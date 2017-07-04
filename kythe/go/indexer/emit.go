/*
 * Copyright 2016 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package indexer

import (
	"context"
	"fmt"
	"go/ast"
	"go/token"
	"go/types"
	"log"
	"net/url"
	"path"
	"strconv"
	"strings"

	"github.com/golang/protobuf/proto"
	"golang.org/x/tools/go/types/typeutil"

	"kythe.io/kythe/go/extractors/govname"
	"kythe.io/kythe/go/util/metadata"
	"kythe.io/kythe/go/util/schema/edges"
	"kythe.io/kythe/go/util/schema/facts"
	"kythe.io/kythe/go/util/schema/nodes"

	spb "kythe.io/kythe/proto/storage_proto"
)

// EmitOptions control the behaviour of the Emit function. A nil options
// pointer provides default values.
type EmitOptions struct {
	// If true, emit nodes for standard library packages when they are first
	// encountered. This is helpful if you want to index a package in isolation
	// where data for the standard library are not available.
	EmitStandardLibs bool

	// If true, emit code facts containing MarkedSource messages.
	EmitMarkedSource bool

	// If true, emit linkages specified by metadata rules.
	EmitLinkages bool

	// If set, use this as the base URL for links to godoc.  The import path is
	// appended to the path of this URL to obtain the target URL to link to.
	DocBase *url.URL
}

// shouldEmit reports whether the indexer should emit a node for the given
// vname.  Presently this is true if vname denotes a standard library and the
// corresponding option is enabled.
func (e *EmitOptions) shouldEmit(vname *spb.VName) bool {
	return e != nil && e.EmitStandardLibs && govname.IsStandardLibrary(vname)
}

// docURL returns a documentation URL for the specified package, if one is
// specified by the options, or "" if not.
func (e *EmitOptions) docURL(pi *PackageInfo) string {
	if e != nil && e.DocBase != nil {
		u := *e.DocBase
		u.Path = path.Join(u.Path, pi.ImportPath)
		return u.String()
	}
	return ""
}

// An impl records that a type A implements an interface B.
type impl struct{ A, B types.Object }

// Emit generates Kythe facts and edges to represent pi, and writes them to
// sink. In case of errors, processing continues as far as possible before the
// first error encountered is reported.
func (pi *PackageInfo) Emit(ctx context.Context, sink Sink, opts *EmitOptions) error {
	e := &emitter{
		ctx:  ctx,
		pi:   pi,
		sink: sink,
		opts: opts,
		impl: make(map[impl]bool),
	}

	// Emit a node to represent the package as a whole.
	e.writeFact(pi.VName, facts.NodeKind, nodes.Package)
	if url := e.opts.docURL(pi); url != "" {
		e.writeFact(pi.VName, facts.DocURI, url)
	}

	// Emit facts for all the source files claimed by this package.
	for file, text := range pi.SourceText {
		vname := pi.FileVName(file)
		e.writeFact(vname, facts.NodeKind, nodes.File)
		e.writeFact(vname, facts.Text, text)
		// All Go source files are encoded as UTF-8, which is the default.

		e.writeEdge(vname, pi.VName, edges.ChildOf)
	}

	// Traverse the AST of each file in the package for xref entries.
	for _, file := range pi.Files {
		e.writeDoc(file.Doc, pi.VName)                        // capture package comments
		e.writeRef(file.Name, pi.VName, edges.DefinesBinding) // define a binding for the package
		ast.Walk(newASTVisitor(func(node ast.Node, stack stackFunc) bool {
			switch n := node.(type) {
			case *ast.Ident:
				e.visitIdent(n, stack)
			case *ast.FuncDecl:
				e.visitFuncDecl(n, stack)
			case *ast.FuncLit:
				e.visitFuncLit(n, stack)
			case *ast.ValueSpec:
				e.visitValueSpec(n, stack)
			case *ast.TypeSpec:
				e.visitTypeSpec(n, stack)
			case *ast.ImportSpec:
				e.visitImportSpec(n, stack)
			case *ast.AssignStmt:
				e.visitAssignStmt(n, stack)
			case *ast.RangeStmt:
				e.visitRangeStmt(n, stack)
			case *ast.CompositeLit:
				e.visitCompositeLit(n, stack)
			}
			return true
		}), file)
	}

	// Emit edges from each named type to the interface types it satisfies, for
	// those interface types that are known to this compiltion.
	e.emitSatisfactions()

	// TODO(fromberger): Add diagnostics for type-checker errors.
	for _, err := range pi.Errors {
		log.Printf("WARNING: Type resolution error: %v", err)
	}
	return e.firstErr
}

type emitter struct {
	ctx      context.Context
	pi       *PackageInfo
	sink     Sink
	opts     *EmitOptions
	impl     map[impl]bool                        // see checkImplements
	rmap     map[*ast.File]map[int]metadata.Rules // see applyRules
	firstErr error
}

// visitIdent handles referring identifiers. Declaring identifiers are handled
// as part of their parent syntax.
func (e *emitter) visitIdent(id *ast.Ident, stack stackFunc) {
	obj := e.pi.Info.Uses[id]
	if obj == nil {
		// Defining identifiers are handled by their parent nodes.
		return
	}

	target := e.pi.ObjectVName(obj)
	e.writeRef(id, target, edges.Ref)
	if call, ok := isCall(id, obj, stack); ok {
		callAnchor := e.writeRef(call, target, edges.RefCall)

		// Paint an edge to the function blamed for the call, or if there is
		// none then to the package initializer.
		e.writeEdge(callAnchor, e.callContext(stack).vname, edges.ChildOf)
	}
}

// visitFuncDecl handles function and method declarations and their parameters.
func (e *emitter) visitFuncDecl(decl *ast.FuncDecl, stack stackFunc) {
	info := &funcInfo{vname: new(spb.VName)}
	e.pi.function[decl] = info

	// Get the type of this function, even if its name is blank.
	obj, _ := e.pi.Info.Defs[decl.Name].(*types.Func)
	if obj == nil {
		return // a redefinition, for example
	}

	// Special case: There may be multiple package-level init functions, so
	// override the normal signature generation to include a discriminator.
	if decl.Recv == nil && obj.Name() == "init" {
		e.pi.numInits++
		e.pi.sigs[obj] = fmt.Sprintf("%s#%d", e.pi.Signature(obj), e.pi.numInits)
	}

	info.vname = e.mustWriteBinding(decl.Name, nodes.Function, nil)
	e.writeDef(decl, info.vname)
	e.writeDoc(decl.Doc, info.vname)

	// For concrete methods: Emit the receiver if named, and connect the method
	// to its declaring type.
	sig := obj.Type().(*types.Signature)
	if sig.Recv() != nil {
		// The receiver is treated as parameter 0.
		if names := decl.Recv.List[0].Names; names != nil {
			if recv := e.writeBinding(names[0], nodes.Variable, info.vname); recv != nil {
				e.writeEdge(info.vname, recv, edges.ParamIndex(0))
			}
		}

		// The method should be a child of its (named) enclosing type.
		if named, _ := deref(sig.Recv().Type()).(*types.Named); named != nil {
			base := e.pi.ObjectVName(named.Obj())
			e.writeEdge(info.vname, base, edges.ChildOf)
		}
	}
	e.emitParameters(decl.Type, sig, info)
}

// visitFuncLit handles function literals and their parameters.  The signature
// for a function literal is named relative to the signature of its parent
// function, or the file scope if the literal is at the top level.
func (e *emitter) visitFuncLit(flit *ast.FuncLit, stack stackFunc) {
	fi := e.callContext(stack)
	if fi == nil {
		log.Panic("Function literal without a context: ", flit)
	}

	fi.numAnons++
	info := &funcInfo{vname: proto.Clone(fi.vname).(*spb.VName)}
	info.vname.Language = govname.Language
	info.vname.Signature += "$" + strconv.Itoa(fi.numAnons)
	e.pi.function[flit] = info
	e.writeDef(flit, info.vname)
	e.writeFact(info.vname, facts.NodeKind, nodes.Function)

	if sig, ok := e.pi.Info.Types[flit].Type.(*types.Signature); ok {
		e.emitParameters(flit.Type, sig, info)
	}
}

// visitValueSpec handles variable and constant bindings.
func (e *emitter) visitValueSpec(spec *ast.ValueSpec, stack stackFunc) {
	kind := nodes.Variable
	if stack(1).(*ast.GenDecl).Tok == token.CONST {
		kind = nodes.Constant
	}
	doc := specComment(spec, stack)
	for _, id := range spec.Names {
		target := e.writeBinding(id, kind, e.nameContext(stack))
		if target == nil {
			continue // type error (reported elsewhere)
		}
		e.writeDoc(doc, target)
	}

	// Handle fields of anonymous struct types declared in situ.
	for _, v := range spec.Values {
		if lit, ok := v.(*ast.CompositeLit); ok {
			e.emitAnonFields(lit.Type)
		}
	}
}

// visitTypeSpec handles type declarations, including the bindings for fields
// of struct types and methods of interfaces.
func (e *emitter) visitTypeSpec(spec *ast.TypeSpec, stack stackFunc) {
	obj, _ := e.pi.Info.Defs[spec.Name]
	if obj == nil {
		return // type error
	}
	target := e.mustWriteBinding(spec.Name, "", e.nameContext(stack))
	e.writeDef(spec, target)
	e.writeDoc(specComment(spec, stack), target)

	// Emit type-specific structure.
	switch t := obj.Type().Underlying().(type) {
	case *types.Struct:
		e.writeFact(target, facts.NodeKind, nodes.Record)
		e.writeFact(target, facts.Subkind, nodes.Struct)
		// Add parent edges for all fields, including promoted ones.
		for i, n := 0, t.NumFields(); i < n; i++ {
			e.writeEdge(e.pi.ObjectVName(t.Field(i)), target, edges.ChildOf)
		}

		// Add bindings for the explicitly-named fields in this declaration.
		// Parent edges were already added, so skip them here.
		if st, ok := spec.Type.(*ast.StructType); ok {
			mapFields(st.Fields, func(i int, id *ast.Ident) {
				target := e.writeVarBinding(id, nodes.Field, nil)
				e.writeDoc(st.Fields.List[i].Doc, target)
			})

			// Handle anonymous fields. Such fields behave as if they were
			// named by the base identifier of their type.
			for _, field := range st.Fields.List {
				if len(field.Names) != 0 {
					continue // already handled above
				}
				id, ok := e.pi.findFieldName(field.Type)
				obj := e.pi.Info.Defs[id]
				if ok && obj != nil {
					// Don't write a fresh anchor here; we already wrote one as
					// part of the ref to the type, and we don't want duplicate
					// outputs.
					anchor := e.pi.AnchorVName(e.pi.Span(id))
					target := e.pi.ObjectVName(obj)
					e.writeEdge(anchor, target, edges.DefinesBinding)
					e.writeFact(target, facts.NodeKind, nodes.Variable)
					e.writeFact(target, facts.Subkind, nodes.Field)
					e.writeDoc(field.Doc, target)
				}
			}
		}

	case *types.Interface:
		e.writeFact(target, facts.NodeKind, nodes.Interface)
		// Add parent edges for all methods, including inherited ones.
		for i, n := 0, t.NumMethods(); i < n; i++ {
			e.writeEdge(e.pi.ObjectVName(t.Method(i)), target, edges.ChildOf)
		}
		// Mark the interface as an extension of any embedded interfaces.
		for i, n := 0, t.NumEmbeddeds(); i < n; i++ {
			if eobj := t.Embedded(i).Obj(); e.checkImplements(obj, eobj) {
				e.writeEdge(target, e.pi.ObjectVName(eobj), edges.Extends)
			}
		}

		// Add bindings for the explicitly-named methods in this declaration.
		// Parent edges were already added, so skip them here.
		if it, ok := spec.Type.(*ast.InterfaceType); ok {
			mapFields(it.Methods, func(_ int, id *ast.Ident) {
				e.writeBinding(id, nodes.Function, nil)
			})
		}

	default:
		// We model a newtype form whose underlying type is not already a
		// struct (e.g., "type Foo int") as if it were a record with a single
		// unexported field of the underlying type. That is not really what Go
		// does, but it is close enough for the graph model to work. Since
		// there is no actual field declaration, however, we don't emit that.
		e.writeFact(target, facts.NodeKind, nodes.Record)
		e.writeFact(target, facts.Subkind, nodes.Type)
	}
}

// visitImportSpec handles references to imported packages.
func (e *emitter) visitImportSpec(spec *ast.ImportSpec, stack stackFunc) {
	var (
		ipath, _ = strconv.Unquote(spec.Path.Value)
		pkg      = e.pi.Dependencies[ipath]
		target   = e.pi.PackageVName[pkg]
	)
	if target == nil {
		log.Printf("Unable to resolve import path %q", ipath)
		return
	}

	e.writeRef(spec.Path, target, edges.RefImports)
	if e.opts.shouldEmit(target) && !e.pi.standardLib.Contains(ipath) {
		e.writeFact(target, facts.NodeKind, nodes.Package)
		e.pi.standardLib.Add(ipath)
	}
}

// visitAssignStmt handles bindings introduced by short-declaration syntax in
// assignment statments, e.g., "x, y := 1, 2".
func (e *emitter) visitAssignStmt(stmt *ast.AssignStmt, stack stackFunc) {
	if stmt.Tok != token.DEFINE {
		return // no new bindings in this statement
	}

	// Not all the names in a short declaration assignment may be defined here.
	// We only add bindings for newly-defined ones, of which there must be at
	// least one in a well-typed program.
	up := e.nameContext(stack)
	for _, expr := range stmt.Lhs {
		if id, _ := expr.(*ast.Ident); id != nil {
			// Add a binding only if this is the definition site for the name.
			if obj := e.pi.Info.Defs[id]; obj != nil && obj.Pos() == id.Pos() {
				e.mustWriteBinding(id, nodes.Variable, up)
			}
		}
	}

	// TODO(fromberger): Add information about initializers where available.
}

// visitRangeStmt handles the bindings introduced by a for ... range statement.
func (e *emitter) visitRangeStmt(stmt *ast.RangeStmt, stack stackFunc) {
	if stmt.Tok != token.DEFINE {
		return // no new bindings in this statement
	}

	// In a well-typed program, the key and value will always be identifiers.
	up := e.nameContext(stack)
	if key, _ := stmt.Key.(*ast.Ident); key != nil {
		e.writeBinding(key, nodes.Variable, up)
	}
	if val, _ := stmt.Value.(*ast.Ident); val != nil {
		e.writeBinding(val, nodes.Variable, up)
	}
}

// visitCompositeLit handles references introduced by positional initializers
// in composite literals that construct (pointer to) struct values. Named
// initializers are handled separately.
func (e *emitter) visitCompositeLit(expr *ast.CompositeLit, stack stackFunc) {
	if len(expr.Elts) == 0 {
		return // no fields to initialize
	}

	tv, ok := e.pi.Info.Types[expr]
	if !ok {
		log.Printf("WARNING: Unable to determine composite literal type (%s)", e.pi.FileSet.Position(expr.Pos()))
		return
	}
	sv, ok := deref(tv.Type.Underlying()).(*types.Struct)
	if !ok {
		return // non-struct type, e.g. a slice; nothing to do here
	}

	if n := sv.NumFields(); n < len(expr.Elts) {
		// Embedded struct fields from an imported package may not appear in
		// the list if the import did not succeed.  To remain robust against
		// such cases, don't try to read into the fields of a struct type if
		// the counts don't line up. The information we emit will still be
		// correct, we'll just miss some initializers.
		log.Printf("ERROR: Struct has %d fields but %d initializers (skipping)", n, len(expr.Elts))
		return
	}
	for i, elt := range expr.Elts {
		// The keys for key-value initializers are handled upstream of us, so
		// we need only handle the values.
		switch t := elt.(type) {
		case *ast.KeyValueExpr:
			e.emitPosRef(t.Value, sv.Field(i), edges.RefInit)
		default:
			e.emitPosRef(t, sv.Field(i), edges.RefInit)
		}
	}
}

// emitPosRef emits a zero-width anchor at the start of loc, pointing to obj.
func (e *emitter) emitPosRef(loc ast.Node, obj types.Object, kind string) {
	target := e.pi.ObjectVName(obj)
	file, start, end := e.pi.Span(loc)
	anchor := e.pi.AnchorVName(file, start, end)
	e.writeAnchor(anchor, start, end)
	e.writeEdge(anchor, target, kind)
}

// emitParameters emits parameter edges for the parameters of a function type,
// given the type signature and info of the enclosing declaration or function
// literal.
func (e *emitter) emitParameters(ftype *ast.FuncType, sig *types.Signature, info *funcInfo) {
	paramIndex := 0

	// If there is a receiver, it is treated as param.0.
	if sig.Recv() != nil {
		paramIndex++
	}

	// Emit bindings and parameter edges for the parameters.
	mapFields(ftype.Params, func(i int, id *ast.Ident) {
		if sig.Params().At(i) != nil {
			if param := e.writeBinding(id, nodes.Variable, info.vname); param != nil {
				e.writeEdge(info.vname, param, edges.ParamIndex(paramIndex))
				e.emitAnonFields(ftype.Params.List[i].Type)
			}
		}
		paramIndex++
	})
	// Emit bindings for any named result variables.
	// Results are not considered parameters.
	mapFields(ftype.Results, func(i int, id *ast.Ident) {
		e.writeBinding(id, nodes.Variable, info.vname)
	})
}

// emitAnonFields checks whether expr denotes an anonymous struct type, and if
// so emits bindings for the fields of that struct. The resulting fields do not
// parent to the struct, since it has no referential identity; but we do
// capture documentation in the unlikely event someone wrote any.
func (e *emitter) emitAnonFields(expr ast.Expr) {
	if st, ok := expr.(*ast.StructType); ok {
		mapFields(st.Fields, func(i int, id *ast.Ident) {
			target := e.writeVarBinding(id, nodes.Field, nil) // no parent
			e.writeDoc(st.Fields.List[i].Doc, target)
		})
	}
}

// An override represents the relationship that x overrides y.
type override struct {
	x, y types.Object
}

// overrides represents a set of override relationships we've already generated.
type overrides map[override]bool

// seen reports whether an x overrides y was already cached, and if not adds it
// to the set.
func (o overrides) seen(x, y types.Object) bool {
	ov := override{x: x, y: y}
	ok := o[ov]
	if !ok {
		o[ov] = true
	}
	return ok
}

// emitSatisfactions visits each named type known through the compilation being
// indexed, and emits edges connecting it to any known interfaces its method
// set satisfies.
func (e *emitter) emitSatisfactions() {
	// Find the names of all defined types mentioned in this compilation.
	var allNames []*types.TypeName

	// For the current source package, use all names, even local ones.
	for _, obj := range e.pi.Info.Defs {
		if obj, ok := obj.(*types.TypeName); ok {
			if _, ok := obj.Type().(*types.Named); ok {
				allNames = append(allNames, obj)
			}
		}
	}

	// For dependencies, we only have access to package-level types, not those
	// defined by inner scopes.
	for _, pkg := range e.pi.Dependencies {
		scope := pkg.Scope()
		for _, name := range scope.Names() {
			if obj, ok := scope.Lookup(name).(*types.TypeName); ok {
				if _, ok := obj.Type().(*types.Named); ok {
					allNames = append(allNames, obj)
				}
			}
		}
	}

	// Cache the method set of each named type in this package.
	var msets typeutil.MethodSetCache
	// Cache the overrides we've noticed to avoid duplicate entries.
	cache := make(overrides)
	for _, xobj := range allNames {
		if xobj.Pkg() != e.pi.Package {
			continue // not from this package
		}

		// Check whether x is a named type with methods; if not, skip it.
		x := xobj.Type()
		ximset := typeutil.IntuitiveMethodSet(x, &msets)
		if len(ximset) == 0 {
			continue // no methods to consider
		}

		// N.B. This implementation is quadratic in the number of visible
		// interfaces, but that's probably OK since are only considering a
		// single compilation.

		xmset := msets.MethodSet(x)
		for _, yobj := range allNames {
			if xobj == yobj {
				continue
			}

			y := yobj.Type()
			ymset := msets.MethodSet(y)

			ifx, ify := isInterface(x), isInterface(y)
			switch {
			case ifx && ify && ymset.Len() > 0:
				if types.AssignableTo(x, y) {
					e.writeSatisfies(xobj, yobj)
				}
				if types.AssignableTo(y, x) {
					e.writeSatisfies(yobj, xobj)
				}

			case ifx:
				// y is a concrete type
				if types.AssignableTo(y, x) {
					e.writeSatisfies(yobj, xobj)
				} else if py := types.NewPointer(y); types.AssignableTo(py, x) {
					e.writeSatisfies(yobj, xobj)
					// TODO(fromberger): Do we want this case?
				}

			case ify && ymset.Len() > 0:
				// x is a concrete type
				if types.AssignableTo(x, y) {
					e.writeSatisfies(xobj, yobj)
				} else if px := types.NewPointer(x); types.AssignableTo(px, y) {
					e.writeSatisfies(xobj, yobj)
					// TODO(fromberger): Do we want this case?
				}
				e.emitOverrides(xmset, ymset, cache)

			default:
				// Both x and y are concrete.
			}
		}
	}
}

// Add xm-(overrides)-ym for each concrete method xm with a corresponding
// abstract method ym.
func (e *emitter) emitOverrides(xmset, ymset *types.MethodSet, cache overrides) {
	for i, n := 0, ymset.Len(); i < n; i++ {
		ym := ymset.At(i)
		yobj := ym.Obj()
		xm := xmset.Lookup(yobj.Pkg(), yobj.Name())
		if xm == nil {
			continue // this method is not part of the interface we're probing
		}

		xobj := xm.Obj()
		if cache.seen(xobj, yobj) {
			continue
		}

		xvname := e.pi.ObjectVName(xobj)
		yvname := e.pi.ObjectVName(yobj)
		e.writeEdge(xvname, yvname, edges.Overrides)
	}
}

func isInterface(typ types.Type) bool { _, ok := typ.Underlying().(*types.Interface); return ok }

func (e *emitter) check(err error) {
	if err != nil && e.firstErr == nil {
		e.firstErr = err
		log.Printf("ERROR indexing %q: %v", e.pi.ImportPath, err)
	}
}

func (e *emitter) checkImplements(src, tgt types.Object) bool {
	i := impl{A: src, B: tgt}
	if e.impl[i] {
		return false
	}
	e.impl[i] = true
	return true
}

func (e *emitter) writeSatisfies(src, tgt types.Object) {
	if e.checkImplements(src, tgt) {
		e.writeEdge(e.pi.ObjectVName(src), e.pi.ObjectVName(tgt), edges.Satisfies)
	}
}

func (e *emitter) writeFact(src *spb.VName, name, value string) {
	e.check(e.sink.writeFact(e.ctx, src, name, value))
}

func (e *emitter) writeEdge(src, tgt *spb.VName, kind string) {
	e.check(e.sink.writeEdge(e.ctx, src, tgt, kind))
}

func (e *emitter) writeAnchor(src *spb.VName, start, end int) {
	e.check(e.sink.writeAnchor(e.ctx, src, start, end))
}

// writeRef emits an anchor spanning origin and referring to target with an
// edge of the given kind. The vname of the anchor is returned.
func (e *emitter) writeRef(origin ast.Node, target *spb.VName, kind string) *spb.VName {
	file, start, end := e.pi.Span(origin)
	anchor := e.pi.AnchorVName(file, start, end)
	e.writeAnchor(anchor, start, end)
	e.writeEdge(anchor, target, kind)

	// Check whether we are intended to emit metadata linkage edges, and if so,
	// whether there are any to process.
	e.applyRules(file, start, end, kind, func(rule metadata.Rule) {
		if rule.Reverse {
			e.writeEdge(rule.VName, target, rule.EdgeOut)
		} else {
			e.writeEdge(target, rule.VName, rule.EdgeOut)
		}
	})

	return anchor
}

// mustWriteBinding is as writeBinding, but panics if id does not resolve.  Use
// this in cases where the object is known already to exist.
func (e *emitter) mustWriteBinding(id *ast.Ident, kind string, parent *spb.VName) *spb.VName {
	if target := e.writeBinding(id, kind, parent); target != nil {
		return target
	}
	panic("unresolved definition") // logged in writeBinding
}

// writeVarBinding is as writeBinding, assuming the kind is "variable".
// If subkind != "", it is also emitted as a subkind.
func (e *emitter) writeVarBinding(id *ast.Ident, subkind string, parent *spb.VName) *spb.VName {
	vname := e.writeBinding(id, nodes.Variable, parent)
	if vname != nil && subkind != "" {
		e.writeFact(vname, facts.Subkind, subkind)
	}
	return vname
}

// writeBinding emits a node of the specified kind for the target of id.  If
// the identifier is not "_", an anchor for a binding definition of the target
// is also emitted at id. If parent != nil, the target is also recorded as its
// child. The target vname is returned.
func (e *emitter) writeBinding(id *ast.Ident, kind string, parent *spb.VName) *spb.VName {
	obj := e.pi.Info.Defs[id]
	if obj == nil {
		loc := e.pi.FileSet.Position(id.Pos())
		log.Printf("ERROR: Missing definition for id %q at %s", id.Name, loc)
		return nil
	}
	target := e.pi.ObjectVName(obj)
	if kind != "" {
		e.writeFact(target, facts.NodeKind, kind)
	}
	if id.Name != "_" {
		e.writeRef(id, target, edges.DefinesBinding)
	}
	if parent != nil {
		e.writeEdge(target, parent, edges.ChildOf)
	}
	if e.opts != nil && e.opts.EmitMarkedSource {
		if ms := e.pi.MarkedSource(obj); ms != nil {
			bits, err := proto.Marshal(ms)
			if err != nil {
				log.Printf("ERROR: Unable to marshal marked source: %v", err)
			} else {
				e.writeFact(target, facts.Code, string(bits))
			}
		}
	}
	return target
}

// writeDef emits a spanning anchor and defines edge for the specified node.
// This function does not create the target node.
func (e *emitter) writeDef(node ast.Node, target *spb.VName) { e.writeRef(node, target, edges.Defines) }

// writeDoc adds associations between comment groups and a documented node.
func (e *emitter) writeDoc(comments *ast.CommentGroup, target *spb.VName) {
	if comments == nil || len(comments.List) == 0 || target == nil {
		return
	}
	var lines []string
	for _, comment := range comments.List {
		lines = append(lines, trimComment(comment.Text))
	}
	docNode := proto.Clone(target).(*spb.VName)
	docNode.Signature += " doc"
	e.writeFact(docNode, facts.NodeKind, nodes.Doc)
	e.writeFact(docNode, facts.Text, strings.Join(lines, "\n"))
	e.writeEdge(docNode, target, edges.Documents)
}

// isCall reports whether id is a call to obj.  This holds if id is in call
// position ("id(...") or is the RHS of a selector in call position
// ("x.id(...)"). If so, the nearest enclosing call expression is also
// returned.
//
// This will not match if there are redundant parentheses in the expression.
func isCall(id *ast.Ident, obj types.Object, stack stackFunc) (*ast.CallExpr, bool) {
	if _, ok := obj.(*types.Func); ok {
		if call, ok := stack(1).(*ast.CallExpr); ok && call.Fun == id {
			return call, true // id(...)
		}
		if sel, ok := stack(1).(*ast.SelectorExpr); ok && sel.Sel == id {
			if call, ok := stack(2).(*ast.CallExpr); ok && call.Fun == sel {
				return call, true // x.id(...)
			}
		}
	}
	return nil, false
}

// callContext returns funcInfo for the nearest enclosing parent function, not
// including the node itself, or the enclosing package initializer if the node
// is at the top level.
func (e *emitter) callContext(stack stackFunc) *funcInfo {
	for i := 1; ; i++ {
		switch p := stack(i).(type) {
		case *ast.FuncDecl, *ast.FuncLit:
			return e.pi.function[p]
		case nil:
			if e.pi.packageInit == nil {
				// Lazily emit a virtual node to represent the static
				// initializer for top-level expressions in the package.  We
				// only do this if there are expressions that need to be
				// initialized.
				vname := proto.Clone(e.pi.VName).(*spb.VName)
				vname.Signature += ".<init>"
				e.pi.packageInit = &funcInfo{vname: vname}
				e.writeFact(vname, facts.NodeKind, nodes.Function)
				e.writeEdge(vname, e.pi.VName, edges.ChildOf)
			}
			return e.pi.packageInit
		}
	}
}

// nameContext returns the vname for the nearest enclosing parent node, not
// including the node itself, or the enclosing package vname if the node is at
// the top level.
func (e *emitter) nameContext(stack stackFunc) *spb.VName {
	if fi := e.callContext(stack); fi != e.pi.packageInit {
		return fi.vname
	}
	return e.pi.VName
}

// applyRules calls apply for each metadata rule matching the given combination
// of location and kind.
func (e *emitter) applyRules(file *ast.File, start, end int, kind string, apply func(r metadata.Rule)) {
	if e.opts == nil || !e.opts.EmitLinkages {
		return // nothing to do
	} else if e.rmap == nil {
		e.rmap = make(map[*ast.File]map[int]metadata.Rules)
	}

	// Lazily populate a cache of file :: start :: rules mappings, so that we
	// need only scan the rules coincident on the starting point of the range
	// we care about. In almost all cases that will be just one, if any.
	rules, ok := e.rmap[file]
	if !ok {
		rules = make(map[int]metadata.Rules)
		for _, rule := range e.pi.Rules[file] {
			rules[rule.Begin] = append(rules[rule.Begin], rule)
		}
		e.rmap[file] = rules
	}

	for _, rule := range rules[start] {
		if rule.End == end && rule.EdgeIn == kind {
			apply(rule)
		}
	}
}

// A visitFunc visits a node of the Go AST. The function can use stack to
// retrieve AST nodes on the path from the node up to the root.  If the return
// value is true, the children of node are also visited; otherwise they are
// skipped.
type visitFunc func(node ast.Node, stack stackFunc) bool

// A stackFunc returns the ith stack entry above of an AST node, where 0
// denotes the node itself. If the ith entry does not exist, the function
// returns nil.
type stackFunc func(i int) ast.Node

// astVisitor implements ast.Visitor, passing each visited node to a callback
// function.
type astVisitor struct {
	stack []ast.Node
	visit visitFunc
}

func newASTVisitor(f visitFunc) ast.Visitor { return &astVisitor{visit: f} }

// Visit implements the required method of the ast.Visitor interface.
func (w *astVisitor) Visit(node ast.Node) ast.Visitor {
	if node == nil {
		w.stack = w.stack[:len(w.stack)-1] // pop
		return w
	}

	w.stack = append(w.stack, node) // push
	if !w.visit(node, w.parent) {
		return nil
	}
	return w
}

func (w *astVisitor) parent(i int) ast.Node {
	if i >= len(w.stack) {
		return nil
	}
	return w.stack[len(w.stack)-1-i]
}

// deref returns the base type of T if it is a pointer, otherwise T itself.
func deref(T types.Type) types.Type {
	if U, ok := T.Underlying().(*types.Pointer); ok {
		return U.Elem()
	}
	return T
}

// mapFields applies f to each identifier declared in fields.  Each call to f
// is given the offset and the identifier.
func mapFields(fields *ast.FieldList, f func(i int, id *ast.Ident)) {
	if fields == nil {
		return
	}
	for i, field := range fields.List {
		for _, id := range field.Names {
			f(i, id)
		}
	}
}

var escComment = strings.NewReplacer("[", `\[`, "]", `\]`, `\`, `\\`)

// trimComment removes the comment delimiters from a comment.  For single-line
// comments, it also removes a single leading space, if present; for multi-line
// comments it discards leading and trailing whitespace. Brackets and backslash
// characters are escaped per http://www.kythe.io/docs/schema/#doc.
func trimComment(text string) string {
	if single := strings.TrimPrefix(text, "//"); single != text {
		return escComment.Replace(strings.TrimPrefix(single, " "))
	}
	trimmed := strings.TrimSpace(strings.TrimSuffix(strings.TrimPrefix(text, "/*"), "*/"))
	return escComment.Replace(trimmed)
}

// specComment returns the innermost comment associated with spec, or nil.
func specComment(spec ast.Spec, stack stackFunc) *ast.CommentGroup {
	var comment *ast.CommentGroup
	switch t := spec.(type) {
	case *ast.TypeSpec:
		comment = t.Doc
	case *ast.ValueSpec:
		comment = t.Doc
	case *ast.ImportSpec:
		comment = t.Doc
	}
	if comment == nil {
		if t, ok := stack(1).(*ast.GenDecl); ok {
			return t.Doc
		}
	}
	return comment
}
