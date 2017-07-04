/*
 * Copyright 2014 Google Inc. All rights reserved.
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

#include "KytheGraphObserver.h"

#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclFriend.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclOpenMP.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprObjC.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/StmtObjC.h"
#include "clang/AST/StmtOpenMP.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/TemplateName.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/SourceManager.h"
#include "kythe/cxx/common/indexing/KytheGraphRecorder.h"
#include "kythe/cxx/common/language.h"
#include "kythe/cxx/common/path_utils.h"

#include "IndexerASTHooks.h"

DEFINE_bool(fail_on_unimplemented_builtin, false,
            "Fail indexer if we encounter a builtin we do not handle");

namespace kythe {

using clang::SourceLocation;
using llvm::StringRef;

static const char *CompletenessToString(
    KytheGraphObserver::Completeness completeness) {
  switch (completeness) {
    case KytheGraphObserver::Completeness::Definition:
      return "definition";
    case KytheGraphObserver::Completeness::Complete:
      return "complete";
    case KytheGraphObserver::Completeness::Incomplete:
      return "incomplete";
  }
  LOG(FATAL) << "Invalid enumerator passed to CompletenessToString.";
  return "invalid-completeness";
}

static const char *FunctionSubkindToString(
    KytheGraphObserver::FunctionSubkind subkind) {
  switch (subkind) {
    case KytheGraphObserver::FunctionSubkind::None:
      return "none";
    case KytheGraphObserver::FunctionSubkind::Constructor:
      return "constructor";
    case KytheGraphObserver::FunctionSubkind::Destructor:
      return "destructor";
  }
  LOG(FATAL) << "Invalid enumerator passed to FunctionSubkindToString.";
  return "invalid-fn-subkind";
}

kythe::proto::VName KytheGraphObserver::VNameFromFileEntry(
    const clang::FileEntry *file_entry) {
  kythe::proto::VName out_name;
  if (!vfs_->get_vname(file_entry, &out_name)) {
    llvm::StringRef working_directory = vfs_->working_directory();
    llvm::StringRef file_name(file_entry->getName());
    if (file_name.startswith(working_directory)) {
      out_name.set_path(RelativizePath(file_name, working_directory));
    } else {
      out_name.set_path(file_entry->getName());
    }
    out_name.set_corpus(claimant_.corpus());
  }
  return out_name;
}

void KytheGraphObserver::AppendFileBufferSliceHashToStream(
    clang::SourceLocation loc, llvm::raw_ostream &Ostream) {
  // TODO(zarko): Does this mechanism produce sufficiently unique
  // identifiers? Ideally, we would hash the full buffer segment into
  // which `loc` points, then record `loc`'s offset.
  bool was_invalid = false;
  auto *buffer = SourceManager->getCharacterData(loc, &was_invalid);
  size_t offset = SourceManager->getFileOffset(loc);
  if (was_invalid) {
    Ostream << "!invalid[" << offset << "]";
    return;
  }
  auto loc_end = clang::Lexer::getLocForEndOfToken(
      loc, 0 /* offset from end of token */, *SourceManager, *getLangOptions());
  size_t offset_end = SourceManager->getFileOffset(loc_end);
  Ostream << HashToString(
      llvm::hash_value(llvm::StringRef(buffer, offset_end - offset)));
}

void KytheGraphObserver::AppendFullLocationToStream(
    std::vector<clang::FileID> *posted_fileids, clang::SourceLocation loc,
    llvm::raw_ostream &Ostream) {
  if (!loc.isValid()) {
    Ostream << "invalid";
    return;
  }
  if (loc.isFileID()) {
    clang::FileID file_id = SourceManager->getFileID(loc);
    const clang::FileEntry *file_entry =
        SourceManager->getFileEntryForID(file_id);
    // Don't use getPresumedLoc() since we want to ignore #line-style
    // directives.
    if (file_entry) {
      size_t offset = SourceManager->getFileOffset(loc);
      Ostream << offset;
    } else {
      AppendFileBufferSliceHashToStream(loc, Ostream);
    }
    size_t file_id_count = posted_fileids->size();
    // Don't inline the same fileid multiple times.
    // Right now we don't emit preprocessor version information, but we
    // do distinguish between FileIDs for the same FileEntry.
    for (size_t old_id = 0; old_id < file_id_count; ++old_id) {
      if (file_id == (*posted_fileids)[old_id]) {
        Ostream << "@." << old_id;
        return;
      }
    }
    posted_fileids->push_back(file_id);
    if (file_entry) {
      kythe::proto::VName file_vname(VNameFromFileEntry(file_entry));
      if (!file_vname.corpus().empty()) {
        Ostream << file_vname.corpus() << "/";
      }
      if (!file_vname.root().empty()) {
        Ostream << file_vname.root() << "/";
      }
      Ostream << file_vname.path();
    }
  } else {
    AppendFullLocationToStream(posted_fileids,
                               SourceManager->getExpansionLoc(loc), Ostream);
    Ostream << "@";
    AppendFullLocationToStream(posted_fileids,
                               SourceManager->getSpellingLoc(loc), Ostream);
  }
}

void KytheGraphObserver::AppendRangeToStream(llvm::raw_ostream &Ostream,
                                             const Range &Range) {
  std::vector<clang::FileID> posted_fileids;
  // We want to override this here so that the names we use are filtered
  // through the vname definitions we got from the compilation unit.
  AppendFullLocationToStream(&posted_fileids, Range.PhysicalRange.getBegin(),
                             Ostream);
  if (Range.PhysicalRange.getEnd() != Range.PhysicalRange.getBegin()) {
    AppendFullLocationToStream(&posted_fileids, Range.PhysicalRange.getEnd(),
                               Ostream);
  }
  if (Range.Kind == GraphObserver::Range::RangeKind::Wraith) {
    Ostream << Range.Context.ToClaimedString();
  }
}

/// \brief Attempt to associate a `SourceLocation` with a `FileEntry` by
/// searching through the location's macro expansion history.
/// \param loc The location to associate. Any `SourceLocation` is acceptable.
/// \param source_manager The `SourceManager` that generated `loc`.
/// \return a `FileEntry` if one was found, null otherwise.
static const clang::FileEntry *SearchForFileEntry(
    clang::SourceLocation loc, clang::SourceManager *source_manager) {
  clang::FileID file_id = source_manager->getFileID(loc);
  const clang::FileEntry *out = loc.isFileID() && loc.isValid()
                                    ? source_manager->getFileEntryForID(file_id)
                                    : nullptr;
  if (out) {
    return out;
  }
  auto expansion = source_manager->getExpansionLoc(loc);
  if (expansion.isValid() && expansion != loc) {
    out = SearchForFileEntry(expansion, source_manager);
    if (out) {
      return out;
    }
  }
  auto spelling = source_manager->getSpellingLoc(loc);
  if (spelling.isValid() && spelling != loc) {
    out = SearchForFileEntry(spelling, source_manager);
  }
  return out;
}

kythe::proto::VName KytheGraphObserver::VNameFromRange(
    const GraphObserver::Range &range) {
  kythe::proto::VName out_name;
  if (range.Kind == GraphObserver::Range::RangeKind::Implicit) {
    VNameRefFromNodeId(range.Context).Expand(&out_name);
    out_name.mutable_signature()->append("@syntactic");
  } else {
    const clang::SourceRange &source_range = range.PhysicalRange;
    clang::SourceLocation begin = source_range.getBegin();
    clang::SourceLocation end = source_range.getEnd();
    CHECK(begin.isValid());
    if (!end.isValid()) {
      end = begin;
    }
    if (begin.isMacroID()) {
      begin = SourceManager->getExpansionLoc(begin);
    }
    if (end.isMacroID()) {
      end = SourceManager->getExpansionLoc(end);
    }
    if (const clang::FileEntry *file_entry =
            SearchForFileEntry(begin, SourceManager)) {
      out_name.CopyFrom(VNameFromFileEntry(file_entry));
    } else if (range.Kind == GraphObserver::Range::RangeKind::Wraith) {
      VNameRefFromNodeId(range.Context).Expand(&out_name);
    }
    size_t begin_offset = SourceManager->getFileOffset(begin);
    size_t end_offset = SourceManager->getFileOffset(end);
    auto *const signature = out_name.mutable_signature();
    signature->append("@");
    signature->append(std::to_string(begin_offset));
    signature->append(":");
    signature->append(std::to_string(end_offset));
    if (range.Kind == GraphObserver::Range::RangeKind::Wraith) {
      signature->append("@");
      signature->append(range.Context.ToClaimedString());
    }
  }
  out_name.set_language(supported_language::kIndexerLang);
  out_name.set_signature(CompressString(out_name.signature()));
  return out_name;
}

void KytheGraphObserver::RecordSourceLocation(
    const VNameRef &vname, clang::SourceLocation source_location,
    PropertyID offset_id) {
  if (source_location.isMacroID()) {
    source_location = SourceManager->getExpansionLoc(source_location);
  }
  size_t offset = SourceManager->getFileOffset(source_location);
  recorder_->AddProperty(vname, offset_id, offset);
}

void KytheGraphObserver::recordMacroNode(const NodeId &macro_id) {
  recorder_->AddProperty(VNameRefFromNodeId(macro_id), NodeKindID::kMacro);
}

void KytheGraphObserver::recordExpandsRange(const Range &source_range,
                                            const NodeId &macro_id) {
  RecordAnchor(source_range, macro_id, EdgeKindID::kRefExpands,
               Claimability::Claimable);
}

void KytheGraphObserver::recordIndirectlyExpandsRange(const Range &source_range,
                                                      const NodeId &macro_id) {
  RecordAnchor(source_range, macro_id, EdgeKindID::kRefExpandsTransitive,
               Claimability::Claimable);
}

void KytheGraphObserver::recordUndefinesRange(const Range &source_range,
                                              const NodeId &macro_id) {
  RecordAnchor(source_range, macro_id, EdgeKindID::kUndefines,
               Claimability::Claimable);
}

void KytheGraphObserver::recordBoundQueryRange(const Range &source_range,
                                               const NodeId &macro_id) {
  RecordAnchor(source_range, macro_id, EdgeKindID::kRefQueries,
               Claimability::Claimable);
}

void KytheGraphObserver::recordIncludesRange(const Range &source_range,
                                             const clang::FileEntry *File) {
  RecordAnchor(source_range, VNameFromFileEntry(File), EdgeKindID::kRefIncludes,
               Claimability::Claimable);
}

void KytheGraphObserver::recordUserDefinedNode(const NodeId &node,
                                               const llvm::StringRef &kind,
                                               Completeness completeness) {
  VNameRef node_vname = VNameRefFromNodeId(node);
  recorder_->AddProperty(node_vname, PropertyID::kNodeKind, kind);
  recorder_->AddProperty(node_vname, PropertyID::kComplete,
                         CompletenessToString(completeness));
}

void KytheGraphObserver::recordVariableNode(
    const NodeId &node, Completeness completeness, VariableSubkind subkind,
    const MaybeFew<MarkedSource> &marked_source) {
  VNameRef node_vname = VNameRefFromNodeId(node);
  recorder_->AddProperty(node_vname, NodeKindID::kVariable);
  recorder_->AddProperty(node_vname, PropertyID::kComplete,
                         CompletenessToString(completeness));
  switch (subkind) {
    case VariableSubkind::Field:
      recorder_->AddProperty(node_vname, PropertyID::kSubkind, "field");
      break;
    case VariableSubkind::None:
      break;
  }
  AddMarkedSource(node_vname, marked_source);
}

void KytheGraphObserver::recordNamespaceNode(
    const NodeId &node, const MaybeFew<MarkedSource> &marked_source) {
  VNameRef node_vname = VNameRefFromNodeId(node);
  if (written_namespaces_.insert(node.ToClaimedString()).second) {
    recorder_->AddProperty(node_vname, NodeKindID::kPackage);
    recorder_->AddProperty(node_vname, PropertyID::kSubkind, "namespace");
    AddMarkedSource(node_vname, marked_source);
  }
}

void KytheGraphObserver::RecordRange(const proto::VName &anchor_name,
                                     const GraphObserver::Range &range) {
  if (!deferring_nodes_ || deferred_anchors_.insert(range).second) {
    VNameRef anchor_name_ref(anchor_name);
    recorder_->AddProperty(anchor_name_ref, NodeKindID::kAnchor);
    if (range.Kind == GraphObserver::Range::RangeKind::Implicit) {
      recorder_->AddProperty(anchor_name_ref, PropertyID::kSubkind, "implicit");
    } else {
      RecordSourceLocation(anchor_name_ref, range.PhysicalRange.getBegin(),
                           PropertyID::kLocationStartOffset);
      RecordSourceLocation(anchor_name_ref, range.PhysicalRange.getEnd(),
                           PropertyID::kLocationEndOffset);
    }
    if (range.Kind == GraphObserver::Range::RangeKind::Wraith) {
      recorder_->AddEdge(anchor_name_ref, EdgeKindID::kChildOfContext,
                         VNameRefFromNodeId(range.Context));
    }
  }
}

void KytheGraphObserver::MetaHookDefines(const MetadataFile &meta,
                                         const VNameRef &anchor,
                                         unsigned range_begin,
                                         unsigned range_end,
                                         const VNameRef &def) {
  auto rules = meta.rules().equal_range(range_begin);
  for (auto rule = rules.first; rule != rules.second; ++rule) {
    if (rule->second.begin == range_begin && rule->second.end == range_end &&
        (rule->second.edge_in == "/kythe/edge/defines" ||
         rule->second.edge_in == "/kythe/edge/defines/binding")) {
      EdgeKindID edge_kind;
      if (of_spelling(rule->second.edge_out, &edge_kind)) {
        if (rule->second.reverse_edge) {
          recorder_->AddEdge(VNameRef(rule->second.vname), edge_kind, def);
        } else {
          recorder_->AddEdge(def, edge_kind, VNameRef(rule->second.vname));
        }
      } else {
        fprintf(stderr, "Unknown edge kind %s from metadata\n",
                rule->second.edge_out.c_str());
      }
    }
  }
}
void KytheGraphObserver::RecordAnchor(
    const GraphObserver::Range &source_range,
    const GraphObserver::NodeId &primary_anchored_to,
    EdgeKindID anchor_edge_kind, Claimability cl) {
  CHECK(!file_stack_.empty());
  if (drop_redundant_wraiths_ &&
      !range_edges_
           .insert(RangeEdge{
               source_range.PhysicalRange, anchor_edge_kind,
               primary_anchored_to,
               RangeEdge::ComputeHash(source_range.PhysicalRange,
                                      anchor_edge_kind, primary_anchored_to)})
           .second) {
    return;
  }
  proto::VName anchor_name = VNameFromRange(source_range);
  if (claimRange(source_range) || claimNode(primary_anchored_to)) {
    RecordRange(anchor_name, source_range);
    cl = Claimability::Unclaimable;
  }
  if (cl == Claimability::Unclaimable) {
    recorder_->AddEdge(VNameRef(anchor_name), anchor_edge_kind,
                       VNameRefFromNodeId(primary_anchored_to));
    if (source_range.Kind == Range::RangeKind::Physical) {
      if (anchor_edge_kind == EdgeKindID::kDefinesBinding) {
        clang::FileID def_file =
            SourceManager->getFileID(source_range.PhysicalRange.getBegin());
        const auto metas = meta_.equal_range(def_file);
        if (metas.first != metas.second) {
          auto begin = source_range.PhysicalRange.getBegin();
          if (begin.isMacroID()) {
            begin = SourceManager->getExpansionLoc(begin);
          }
          auto end = source_range.PhysicalRange.getEnd();
          if (end.isMacroID()) {
            end = SourceManager->getExpansionLoc(end);
          }
          unsigned range_begin = SourceManager->getFileOffset(begin);
          unsigned range_end = SourceManager->getFileOffset(end);
          for (auto meta = metas.first; meta != metas.second; ++meta) {
            MetaHookDefines(*meta->second, VNameRef(anchor_name), range_begin,
                            range_end, VNameRefFromNodeId(primary_anchored_to));
          }
        }
      }
    }
  }
}

void KytheGraphObserver::RecordAnchor(
    const GraphObserver::Range &source_range,
    const kythe::proto::VName &primary_anchored_to, EdgeKindID anchor_edge_kind,
    Claimability cl) {
  CHECK(!file_stack_.empty());
  proto::VName anchor_name = VNameFromRange(source_range);
  if (claimRange(source_range)) {
    RecordRange(anchor_name, source_range);
    cl = Claimability::Unclaimable;
  }
  if (cl == Claimability::Unclaimable) {
    recorder_->AddEdge(VNameRef(anchor_name), anchor_edge_kind,
                       VNameRef(primary_anchored_to));
  }
}

void KytheGraphObserver::recordCallEdge(
    const GraphObserver::Range &source_range, const NodeId &caller_id,
    const NodeId &callee_id) {
  RecordAnchor(source_range, caller_id, EdgeKindID::kChildOf,
               Claimability::Claimable);
  RecordAnchor(source_range, callee_id, EdgeKindID::kRefCall,
               Claimability::Unclaimable);
}

VNameRef KytheGraphObserver::VNameRefFromNodeId(
    const GraphObserver::NodeId &node_id) {
  VNameRef out_ref;
  out_ref.language = llvm::StringRef(supported_language::kIndexerLang);
  if (const auto *token =
          clang::dyn_cast<KytheClaimToken>(node_id.getToken())) {
    token->DecorateVName(&out_ref);
    if (token->language_independent()) {
      out_ref.language = llvm::StringRef();
    }
  }
  out_ref.signature = node_id.IdentityRef();
  return out_ref;
}

void KytheGraphObserver::recordParamEdge(const NodeId &param_of_id,
                                         uint32_t ordinal,
                                         const NodeId &param_id) {
  recorder_->AddEdge(VNameRefFromNodeId(param_of_id), EdgeKindID::kParam,
                     VNameRefFromNodeId(param_id), ordinal);
}

void KytheGraphObserver::recordChildOfEdge(const NodeId &child_id,
                                           const NodeId &parent_id) {
  recorder_->AddEdge(VNameRefFromNodeId(child_id), EdgeKindID::kChildOf,
                     VNameRefFromNodeId(parent_id));
}

void KytheGraphObserver::recordTypeEdge(const NodeId &term_id,
                                        const NodeId &type_id) {
  recorder_->AddEdge(VNameRefFromNodeId(term_id), EdgeKindID::kHasType,
                     VNameRefFromNodeId(type_id));
}

void KytheGraphObserver::recordUpperBoundEdge(const NodeId &TypeNodeId,
                                              const NodeId &TypeBoundNodeId) {
  recorder_->AddEdge(VNameRefFromNodeId(TypeNodeId), EdgeKindID::kBoundedUpper,
                     VNameRefFromNodeId(TypeBoundNodeId));
}

void KytheGraphObserver::recordVariance(const NodeId &TypeNodeId,
                                        const Variance V) {
  std::string Variance;
  switch (V) {
    // KytheGraphObserver prefix shouldn't be necessary but some compiler
    // incantations complain if it is not there.
    case KytheGraphObserver::Variance::Contravariant:
      Variance = "contravariant";
      break;
    case KytheGraphObserver::Variance::Covariant:
      Variance = "covariant";
      break;
    case KytheGraphObserver::Variance::Invariant:
      Variance = "invariant";
      break;
  }
  recorder_->AddProperty(VNameRefFromNodeId(TypeNodeId), PropertyID::kVariance,
                         Variance);
}

void KytheGraphObserver::recordSpecEdge(const NodeId &term_id,
                                        const NodeId &type_id,
                                        Confidence conf) {
  switch (conf) {
    case Confidence::NonSpeculative:
      recorder_->AddEdge(VNameRefFromNodeId(term_id), EdgeKindID::kSpecializes,
                         VNameRefFromNodeId(type_id));
      break;
    case Confidence::Speculative:
      recorder_->AddEdge(VNameRefFromNodeId(term_id),
                         EdgeKindID::kSpecializesSpeculative,
                         VNameRefFromNodeId(type_id));
      break;
  }
}

void KytheGraphObserver::recordInstEdge(const NodeId &term_id,
                                        const NodeId &type_id,
                                        Confidence conf) {
  switch (conf) {
    case Confidence::NonSpeculative:
      recorder_->AddEdge(VNameRefFromNodeId(term_id), EdgeKindID::kInstantiates,
                         VNameRefFromNodeId(type_id));
      break;
    case Confidence::Speculative:
      recorder_->AddEdge(VNameRefFromNodeId(term_id),
                         EdgeKindID::kInstantiatesSpeculative,
                         VNameRefFromNodeId(type_id));
      break;
  }
}

void KytheGraphObserver::recordOverridesEdge(const NodeId &overrider,
                                             const NodeId &base_object) {
  recorder_->AddEdge(VNameRefFromNodeId(overrider), EdgeKindID::kOverrides,
                     VNameRefFromNodeId(base_object));
}

void KytheGraphObserver::recordOverridesRootEdge(const NodeId &overrider,
                                                 const NodeId &root_object) {
  recorder_->AddEdge(VNameRefFromNodeId(overrider), EdgeKindID::kOverridesRoot,
                     VNameRefFromNodeId(root_object));
}

GraphObserver::NodeId KytheGraphObserver::nodeIdForTypeAliasNode(
    const NameId &alias_name, const NodeId &aliased_type) {
  return NodeId(&type_token_, "talias(" + alias_name.ToString() + "," +
                                  aliased_type.ToClaimedString() + ")");
}

GraphObserver::NodeId KytheGraphObserver::recordTypeAliasNode(
    const NameId &alias_name, const NodeId &aliased_type,
    const MaybeFew<NodeId> &root_aliased_type,
    const MaybeFew<MarkedSource> &marked_source) {
  NodeId type_id = nodeIdForTypeAliasNode(alias_name, aliased_type);
  if (!deferring_nodes_ ||
      written_types_.insert(type_id.ToClaimedString()).second) {
    VNameRef type_vname(VNameRefFromNodeId(type_id));
    recorder_->AddProperty(type_vname, NodeKindID::kTAlias);
    AddMarkedSource(type_vname, marked_source);
    VNameRef aliased_type_vname(VNameRefFromNodeId(aliased_type));
    recorder_->AddEdge(type_vname, EdgeKindID::kAliases,
                       VNameRef(aliased_type_vname));
    if (root_aliased_type) {
      VNameRef root_aliased_type_vname(
          VNameRefFromNodeId(root_aliased_type.primary()));
      recorder_->AddEdge(type_vname, EdgeKindID::kAliasesRoot,
                         VNameRef(root_aliased_type_vname));
    }
  }
  return type_id;
}

void KytheGraphObserver::recordDocumentationText(
    const NodeId &node, const std::string &doc_text,
    const std::vector<NodeId> &doc_links) {
  std::string signature = doc_text;
  for (const auto &link : doc_links) {
    signature.push_back(',');
    signature.append(link.ToClaimedString());
  }
  // Force hashing because the serving backend gets upset if certain
  // characters appear in VName fields.
  NodeId doc_id(node.getToken(), CompressString(signature, true));
  VNameRef doc_vname(VNameRefFromNodeId(doc_id));
  if (written_docs_.insert(doc_id.ToClaimedString()).second) {
    recorder_->AddProperty(doc_vname, NodeKindID::kDoc);
    recorder_->AddProperty(doc_vname, PropertyID::kText, doc_text);
    size_t param_index = 0;
    for (const auto &link : doc_links) {
      recorder_->AddEdge(doc_vname, EdgeKindID::kParam,
                         VNameRefFromNodeId(link), param_index++);
    }
  }
  recorder_->AddEdge(doc_vname, EdgeKindID::kDocuments,
                     VNameRefFromNodeId(node));
}

void KytheGraphObserver::recordDocumentationRange(
    const GraphObserver::Range &source_range, const NodeId &node) {
  RecordAnchor(source_range, node, EdgeKindID::kDocuments,
               Claimability::Claimable);
}

void KytheGraphObserver::recordFullDefinitionRange(
    const GraphObserver::Range &source_range, const NodeId &node) {
  RecordAnchor(source_range, node, EdgeKindID::kDefinesFull,
               Claimability::Claimable);
}

void KytheGraphObserver::recordDefinitionBindingRange(
    const GraphObserver::Range &binding_range, const NodeId &node) {
  RecordAnchor(binding_range, node, EdgeKindID::kDefinesBinding,
               Claimability::Claimable);
}

void KytheGraphObserver::recordDefinitionRangeWithBinding(
    const GraphObserver::Range &source_range,
    const GraphObserver::Range &binding_range, const NodeId &node) {
  RecordAnchor(source_range, node, EdgeKindID::kDefinesFull,
               Claimability::Claimable);
  RecordAnchor(binding_range, node, EdgeKindID::kDefinesBinding,
               Claimability::Claimable);
}

void KytheGraphObserver::recordCompletionRange(
    const GraphObserver::Range &source_range, const NodeId &node,
    Specificity spec) {
  RecordAnchor(source_range, node,
               spec == Specificity::UniquelyCompletes
                   ? EdgeKindID::kUniquelyCompletes
                   : EdgeKindID::kCompletes,
               Claimability::Unclaimable);
}

GraphObserver::NodeId KytheGraphObserver::nodeIdForNominalTypeNode(
    const NameId &name_id) {
  // Appending #t to a name produces the VName signature of the nominal
  // type node referring to that name. For example, the VName for a
  // forward-declared class type will look like "C#c#t".
  return NodeId(&type_token_, name_id.ToString() + "#t");
}

GraphObserver::NodeId KytheGraphObserver::recordNominalTypeNode(
    const NameId &name_id, const MaybeFew<MarkedSource> &marked_source,
    const NodeId *parent) {
  NodeId id_out = nodeIdForNominalTypeNode(name_id);
  if (!deferring_nodes_ ||
      written_types_.insert(id_out.ToClaimedString()).second) {
    VNameRef type_vname(VNameRefFromNodeId(id_out));
    AddMarkedSource(type_vname, marked_source);
    recorder_->AddProperty(type_vname, NodeKindID::kTNominal);
    if (parent) {
      recorder_->AddEdge(type_vname, EdgeKindID::kChildOf,
                         VNameRefFromNodeId(*parent));
    }
  }
  return id_out;
}

GraphObserver::NodeId KytheGraphObserver::recordTsigmaNode(
    const std::vector<const NodeId *> &params) {
  std::string identity;
  llvm::raw_string_ostream ostream(identity);
  bool comma = false;
  ostream << "#sigma(";
  for (const auto *next_id : params) {
    if (comma) {
      ostream << ",";
    }
    ostream << next_id->ToClaimedString();
    comma = true;
  }
  ostream << ")";
  GraphObserver::NodeId id_out(&type_token_, ostream.str());
  if (!deferring_nodes_ ||
      written_types_.insert(id_out.ToClaimedString()).second) {
    VNameRef tsigma_vname(VNameRefFromNodeId(id_out));
    recorder_->AddProperty(tsigma_vname, NodeKindID::kTSigma);
    for (uint32_t param_index = 0; param_index < params.size(); ++param_index) {
      recorder_->AddEdge(tsigma_vname, EdgeKindID::kParam,
                         VNameRefFromNodeId(*params[param_index]), param_index);
    }
  }
  return id_out;
}

GraphObserver::NodeId KytheGraphObserver::recordTappNode(
    const NodeId &tycon_id, const std::vector<const NodeId *> &params,
    unsigned FirstDefaultParam) {
  CHECK(FirstDefaultParam <= params.size());
  // We can't just use juxtaposition here because it leads to ambiguity
  // as we can't assume that we have kind information, eg
  //   foo bar baz
  // might be
  //   foo (bar baz)
  // We'll turn it into a C-style function application:
  //   foo(bar,baz) || foo(bar(baz))
  std::string identity;
  llvm::raw_string_ostream ostream(identity);
  bool comma = false;
  ostream << tycon_id.ToClaimedString();
  ostream << "(";
  for (const auto *next_id : params) {
    if (comma) {
      ostream << ",";
    }
    ostream << next_id->ToClaimedString();
    comma = true;
  }
  ostream << ")";
  GraphObserver::NodeId id_out(&type_token_, ostream.str());
  if (!deferring_nodes_ ||
      written_types_.insert(id_out.ToClaimedString()).second) {
    VNameRef tapp_vname(VNameRefFromNodeId(id_out));
    recorder_->AddProperty(tapp_vname, NodeKindID::kTApp);
    if (FirstDefaultParam < params.size()) {
      recorder_->AddProperty(tapp_vname, PropertyID::kParamDefault,
                             FirstDefaultParam);
    }
    recorder_->AddEdge(tapp_vname, EdgeKindID::kParam,
                       VNameRefFromNodeId(tycon_id), 0);
    for (uint32_t param_index = 0; param_index < params.size(); ++param_index) {
      recorder_->AddEdge(tapp_vname, EdgeKindID::kParam,
                         VNameRefFromNodeId(*params[param_index]),
                         param_index + 1);
    }
  }
  return id_out;
}

void KytheGraphObserver::recordEnumNode(const NodeId &node_id,
                                        Completeness completeness,
                                        EnumKind enum_kind) {
  VNameRef node_vname = VNameRefFromNodeId(node_id);
  recorder_->AddProperty(node_vname, NodeKindID::kSum);
  recorder_->AddProperty(node_vname, PropertyID::kComplete,
                         CompletenessToString(completeness));
  recorder_->AddProperty(node_vname, PropertyID::kSubkind,
                         enum_kind == EnumKind::Scoped ? "enumClass" : "enum");
}

void KytheGraphObserver::recordIntegerConstantNode(const NodeId &node_id,
                                                   const llvm::APSInt &Value) {
  VNameRef node_vname(VNameRefFromNodeId(node_id));
  recorder_->AddProperty(node_vname, NodeKindID::kConstant);
  recorder_->AddProperty(node_vname, PropertyID::kText, Value.toString(10));
}

void KytheGraphObserver::recordFunctionNode(
    const NodeId &node_id, Completeness completeness, FunctionSubkind subkind,
    const MaybeFew<MarkedSource> &marked_source) {
  VNameRef node_vname = VNameRefFromNodeId(node_id);
  recorder_->AddProperty(node_vname, NodeKindID::kFunction);
  recorder_->AddProperty(node_vname, PropertyID::kComplete,
                         CompletenessToString(completeness));
  AddMarkedSource(node_vname, marked_source);
  if (subkind != FunctionSubkind::None) {
    recorder_->AddProperty(node_vname, PropertyID::kSubkind,
                           FunctionSubkindToString(subkind));
  }
}

void KytheGraphObserver::recordAbsNode(const NodeId &node_id) {
  recorder_->AddProperty(VNameRefFromNodeId(node_id), NodeKindID::kAbs);
}

void KytheGraphObserver::recordMarkedSource(
    const NodeId &node_id, const MaybeFew<MarkedSource> &marked_source) {
  VNameRef node_vname = VNameRefFromNodeId(node_id);
  AddMarkedSource(node_vname, marked_source);
}

void KytheGraphObserver::recordAbsVarNode(const NodeId &node_id) {
  recorder_->AddProperty(VNameRefFromNodeId(node_id), NodeKindID::kAbsVar);
}

void KytheGraphObserver::recordLookupNode(const NodeId &node_id,
                                          const llvm::StringRef &text) {
  VNameRef node_vname = VNameRefFromNodeId(node_id);
  recorder_->AddProperty(node_vname, NodeKindID::kLookup);
  recorder_->AddProperty(node_vname, PropertyID::kText, text);
  MarkedSource marked_source;
  marked_source.set_kind(MarkedSource::BOX);
  auto *lhs = marked_source.add_child();
  lhs->set_kind(MarkedSource::CONTEXT);
  lhs = lhs->add_child();
  lhs->set_kind(MarkedSource::PARAMETER_LOOKUP_BY_PARAM);
  lhs->set_pre_text("dependent(");
  lhs->set_post_child_text("::");
  lhs->set_post_text(")::");
  auto *rhs = marked_source.add_child();
  rhs->set_kind(MarkedSource::IDENTIFIER);
  rhs->set_pre_text(text.str());
  recorder_->AddMarkedSource(node_vname, marked_source);
}

void KytheGraphObserver::recordInterfaceNode(
    const NodeId &node_id, const MaybeFew<MarkedSource> &marked_source) {
  VNameRef node_vname = VNameRefFromNodeId(node_id);
  recorder_->AddProperty(node_vname, NodeKindID::kInterface);
  AddMarkedSource(node_vname, marked_source);
}

void KytheGraphObserver::recordRecordNode(
    const NodeId &node_id, RecordKind kind, Completeness completeness,
    const MaybeFew<MarkedSource> &marked_source) {
  VNameRef node_vname = VNameRefFromNodeId(node_id);
  recorder_->AddProperty(node_vname, NodeKindID::kRecord);
  switch (kind) {
    case RecordKind::Class:
      recorder_->AddProperty(node_vname, PropertyID::kSubkind, "class");
      break;
    case RecordKind::Struct:
      recorder_->AddProperty(node_vname, PropertyID::kSubkind, "struct");
      break;
    case RecordKind::Union:
      recorder_->AddProperty(node_vname, PropertyID::kSubkind, "union");
      break;
    case RecordKind::Category:
      recorder_->AddProperty(node_vname, PropertyID::kSubkind, "category");
      break;
  };
  recorder_->AddProperty(node_vname, PropertyID::kComplete,
                         CompletenessToString(completeness));
  AddMarkedSource(node_vname, marked_source);
}

void KytheGraphObserver::recordTypeSpellingLocation(
    const GraphObserver::Range &type_source_range, const NodeId &type_id,
    Claimability claimability) {
  RecordAnchor(type_source_range, type_id, EdgeKindID::kRef, claimability);
}

void KytheGraphObserver::recordCategoryExtendsEdge(const NodeId &from,
                                                   const NodeId &to) {
  recorder_->AddEdge(VNameRefFromNodeId(from), EdgeKindID::kExtendsCategory,
                     VNameRefFromNodeId(to));
}

void KytheGraphObserver::recordExtendsEdge(const NodeId &from, const NodeId &to,
                                           bool is_virtual,
                                           clang::AccessSpecifier specifier) {
  switch (specifier) {
    case clang::AccessSpecifier::AS_public:
      recorder_->AddEdge(VNameRefFromNodeId(from),
                         is_virtual ? EdgeKindID::kExtendsPublicVirtual
                                    : EdgeKindID::kExtendsPublic,
                         VNameRefFromNodeId(to));
      break;
    case clang::AccessSpecifier::AS_protected:
      recorder_->AddEdge(VNameRefFromNodeId(from),
                         is_virtual ? EdgeKindID::kExtendsProtectedVirtual
                                    : EdgeKindID::kExtendsProtected,
                         VNameRefFromNodeId(to));
      break;
    case clang::AccessSpecifier::AS_private:
      recorder_->AddEdge(VNameRefFromNodeId(from),
                         is_virtual ? EdgeKindID::kExtendsPrivateVirtual
                                    : EdgeKindID::kExtendsPrivate,
                         VNameRefFromNodeId(to));
      break;
    default:
      recorder_->AddEdge(
          VNameRefFromNodeId(from),
          is_virtual ? EdgeKindID::kExtendsVirtual : EdgeKindID::kExtends,
          VNameRefFromNodeId(to));
  }
}

void KytheGraphObserver::recordDeclUseLocationInDocumentation(
    const GraphObserver::Range &source_range, const NodeId &node) {
  RecordAnchor(source_range, node, EdgeKindID::kRefDoc,
               Claimability::Claimable);
}

void KytheGraphObserver::recordDeclUseLocation(
    const GraphObserver::Range &source_range, const NodeId &node,
    Claimability claimability) {
  RecordAnchor(source_range, node, EdgeKindID::kRef, claimability);
}

GraphObserver::NodeId KytheGraphObserver::getNodeIdForBuiltinType(
    const llvm::StringRef &spelling) {
  const auto &info = builtins_.find(spelling.str());
  if (info == builtins_.end()) {
    if (FLAGS_fail_on_unimplemented_builtin) {
      LOG(FATAL) << "Missing builtin " << spelling.str();
    }
    LOG(ERROR) << "Missing builtin " << spelling.str();
    MarkedSource sig;
    sig.set_kind(MarkedSource::IDENTIFIER);
    sig.set_pre_text(spelling);
    builtins_.emplace(spelling.str(), Builtin{NodeId::CreateUncompressed(
                                                  getDefaultClaimToken(),
                                                  spelling.str() + "#builtin"),
                                              sig, true});
    auto *new_builtin = &builtins_.find(spelling.str())->second;
    EmitBuiltin(new_builtin);
    return new_builtin->node_id;
  }
  if (!info->second.emitted) {
    EmitBuiltin(&info->second);
  }
  return info->second.node_id;
}

void KytheGraphObserver::applyMetadataFile(clang::FileID id,
                                           const clang::FileEntry *file) {
  const llvm::MemoryBuffer *buffer =
      SourceManager->getMemoryBufferForFile(file);
  if (!buffer) {
    fprintf(stderr, "Couldn't get content for %s\n", file->getName());
    return;
  }
  if (auto metadata = meta_supports_->ParseFile(file->getName(), buffer)) {
    meta_.emplace(id, std::move(metadata));
  }
}

void KytheGraphObserver::AppendMainSourceFileIdentifierToStream(
    llvm::raw_ostream &ostream) {
  if (main_source_file_token_) {
    AppendRangeToStream(ostream,
                        Range(main_source_file_loc_, main_source_file_token_));
  }
}

bool KytheGraphObserver::isMainSourceFileRelatedLocation(
    clang::SourceLocation location) {
  // Where was this thing spelled out originally?
  if (location.isInvalid()) {
    return true;
  }
  if (!location.isFileID()) {
    location = SourceManager->getExpansionLoc(location);
    if (!location.isValid() || !location.isFileID()) {
      return true;
    }
  }
  clang::FileID file = SourceManager->getFileID(location);
  if (file.isInvalid()) {
    return true;
  }
  if (const clang::FileEntry *entry = SourceManager->getFileEntryForID(file)) {
    return transitively_reached_through_header_.find(entry->getUniqueID()) ==
           transitively_reached_through_header_.end();
  }
  return true;
}

bool KytheGraphObserver::claimImplicitNode(const std::string &identifier) {
  kythe::proto::VName node_vname;
  node_vname.set_signature(identifier);
  return client_->Claim(claimant_, node_vname);
}

void KytheGraphObserver::finishImplicitNode(const std::string &identifier) {
  // TODO(zarko): Handle this in two phases. This should commmit the claim.
}

bool KytheGraphObserver::claimBatch(
    std::vector<std::pair<std::string, bool>> *pairs) {
  return client_->ClaimBatch(pairs);
}

void KytheGraphObserver::pushFile(clang::SourceLocation blame_location,
                                  clang::SourceLocation source_location) {
  PreprocessorContext previous_context =
      file_stack_.empty() ? starting_context_ : file_stack_.back().context;
  bool has_previous_uid = !file_stack_.empty();
  llvm::sys::fs::UniqueID previous_uid;
  bool in_header = false;
  if (has_previous_uid) {
    previous_uid = file_stack_.back().uid;
    in_header = transitively_reached_through_header_.find(previous_uid) !=
                transitively_reached_through_header_.end();
  }
  file_stack_.push_back(FileState{});
  FileState &state = file_stack_.back();
  state.claimed = true;
  if (source_location.isValid()) {
    if (source_location.isMacroID()) {
      source_location = SourceManager->getExpansionLoc(source_location);
    }
    CHECK(source_location.isFileID());
    clang::FileID file = SourceManager->getFileID(source_location);
    if (file.isInvalid()) {
      // An actually invalid location.
    } else {
      const clang::FileEntry *entry = SourceManager->getFileEntryForID(file);
      if (entry) {
        // An actual file.
        state.vname = state.base_vname = VNameFromFileEntry(entry);
        state.uid = entry->getUniqueID();
        // TODO(zarko): If modules are enabled, check there to see whether
        // `entry` is a textual header.
        if (in_header ||
            (has_previous_uid &&
             !llvm::StringRef(entry->getName()).endswith(".inc"))) {
          transitively_reached_through_header_.insert(state.uid);
        }
        // Attempt to compute the state-amended VName using the state table.
        // If we aren't working under any context, we won't end up making the
        // VName more specific.
        if (file_stack_.size() == 1) {
          // Start state.
          state.context = starting_context_;
        } else if (has_previous_uid && !previous_context.empty() &&
                   blame_location.isValid() && blame_location.isFileID()) {
          unsigned offset = SourceManager->getFileOffset(blame_location);
          const auto path_info = path_to_context_data_.find(previous_uid);
          if (path_info != path_to_context_data_.end()) {
            const auto context_info = path_info->second.find(previous_context);
            if (context_info != path_info->second.end()) {
              const auto offset_info = context_info->second.find(offset);
              if (offset_info != context_info->second.end()) {
                state.context = offset_info->second;
              } else {
                fprintf(stderr,
                        "Warning: when looking for %s[%s]:%u: missing source "
                        "offset\n",
                        vfs_->get_debug_uid_string(previous_uid).c_str(),
                        previous_context.c_str(), offset);
              }
            } else {
              fprintf(stderr,
                      "Warning: when looking for %s[%s]:%u: missing source "
                      "context\n",
                      vfs_->get_debug_uid_string(previous_uid).c_str(),
                      previous_context.c_str(), offset);
            }
          } else {
            fprintf(
                stderr,
                "Warning: when looking for %s[%s]:%u: missing source path\n",
                vfs_->get_debug_uid_string(previous_uid).c_str(),
                previous_context.c_str(), offset);
          }
        }
        state.vname.set_signature(state.context + state.vname.signature());
        if (client_->Claim(claimant_, state.vname)) {
          if (recorded_files_.insert(entry).second) {
            bool was_invalid = false;
            const llvm::MemoryBuffer *buf =
                SourceManager->getMemoryBufferForFile(entry, &was_invalid);
            if (was_invalid || !buf) {
              // TODO(zarko): diagnostic logging.
            } else {
              recorder_->AddFileContent(VNameRef(state.base_vname),
                                        buf->getBuffer());
            }
          }
        } else {
          state.claimed = false;
        }
        KytheClaimToken token;
        token.set_vname(state.vname);
        token.set_rough_claimed(state.claimed);
        claim_checked_files_.emplace(file, token);
        if (state.claimed) {
          KytheClaimToken file_token;
          file_token.set_vname(state.vname);
          file_token.set_rough_claimed(state.claimed);
          file_token.set_language_independent(true);
          claimed_file_specific_tokens_.emplace(file, file_token);
        }
        if (!has_previous_uid) {
          main_source_file_loc_ = source_location;
          main_source_file_token_ = &claim_checked_files_[file];
        }
      } else {
        // A builtin location.
      }
    }
  }
}

void KytheGraphObserver::popFile() {
  CHECK(!file_stack_.empty());
  FileState state = file_stack_.back();
  file_stack_.pop_back();
  if (file_stack_.empty()) {
    deferred_anchors_.clear();
  }
}

void KytheGraphObserver::iterateOverClaimedFiles(
    std::function<bool(clang::FileID, const NodeId &)> iter) {
  for (const auto &file : claimed_file_specific_tokens_) {
    if (!iter(file.first, NodeId(&file.second, ""))) {
      return;
    }
  }
}

bool KytheGraphObserver::claimRange(const GraphObserver::Range &range) {
  return (range.Kind == GraphObserver::Range::RangeKind::Wraith &&
          claimNode(range.Context)) ||
         claimLocation(range.PhysicalRange.getBegin());
}

bool KytheGraphObserver::claimLocation(clang::SourceLocation source_location) {
  if (!source_location.isValid()) {
    return true;
  }
  if (source_location.isMacroID()) {
    source_location = SourceManager->getExpansionLoc(source_location);
  }
  CHECK(source_location.isFileID());
  clang::FileID file = SourceManager->getFileID(source_location);
  if (file.isInvalid()) {
    return true;
  }
  auto token = claim_checked_files_.find(file);
  return token != claim_checked_files_.end() ? token->second.rough_claimed()
                                             : false;
}

void KytheGraphObserver::AddContextInformation(
    const std::string &path, const PreprocessorContext &context,
    unsigned offset, const PreprocessorContext &dest_context) {
  auto found_file = vfs_->status(path);
  if (found_file) {
    path_to_context_data_[found_file->getUniqueID()][context][offset] =
        dest_context;
  } else {
    fprintf(stderr, "WARNING: Path %s could not be mapped to a VFS record.\n",
            path.c_str());
  }
}

KytheClaimToken *KytheGraphObserver::getClaimTokenForLocation(
    clang::SourceLocation source_location) {
  if (!source_location.isValid()) {
    return &default_token_;
  }
  if (source_location.isMacroID()) {
    source_location = SourceManager->getExpansionLoc(source_location);
  }
  CHECK(source_location.isFileID());
  clang::FileID file = SourceManager->getFileID(source_location);
  if (file.isInvalid()) {
    return &default_token_;
  }
  auto token = claim_checked_files_.find(file);
  return token != claim_checked_files_.end() ? &token->second : &default_token_;
}

KytheClaimToken *KytheGraphObserver::getClaimTokenForRange(
    const clang::SourceRange &range) {
  return getClaimTokenForLocation(range.getBegin());
}

KytheClaimToken *KytheGraphObserver::getAnonymousNamespaceClaimToken(
    clang::SourceLocation loc) {
  if (isMainSourceFileRelatedLocation(loc)) {
    CHECK(main_source_file_token_ != nullptr);
    return main_source_file_token_;
  }
  return getNamespaceClaimToken(loc);
}

KytheClaimToken *KytheGraphObserver::getNamespaceClaimToken(
    clang::SourceLocation loc) {
  auto *file_token = getClaimTokenForLocation(loc);
  auto token = namespace_tokens_.find(file_token);
  if (token != namespace_tokens_.end()) {
    return &token->second;
  }
  proto::VName vname;
  vname.set_corpus(file_token->vname().corpus());
  KytheClaimToken new_token;
  new_token.set_vname(vname);
  new_token.set_rough_claimed(file_token->rough_claimed());
  namespace_tokens_.emplace(file_token, new_token);
  return &namespace_tokens_.find(file_token)->second;
}

void KytheGraphObserver::RegisterBuiltins() {
  auto RegisterBuiltin = [&](const std::string &name,
                             const MarkedSource &marked_source) {
    builtins_.emplace(std::make_pair(
        name, Builtin{NodeId::CreateUncompressed(getDefaultClaimToken(),
                                                 name + "#builtin"),
                      marked_source, false}));
  };
  auto RegisterTokenBuiltin = [&](const std::string &name,
                                  const std::string &token) {
    MarkedSource sig;
    sig.set_kind(MarkedSource::IDENTIFIER);
    sig.set_pre_text(token);
    RegisterBuiltin(name, sig);
  };
  RegisterTokenBuiltin("void", "void");
  RegisterTokenBuiltin("bool", "bool");
  RegisterTokenBuiltin("_Bool", "_Bool");
  RegisterTokenBuiltin("signed char", "signed char");
  RegisterTokenBuiltin("char", "char");
  RegisterTokenBuiltin("char16_t", "char16_t");
  RegisterTokenBuiltin("char32_t", "char32_t");
  RegisterTokenBuiltin("wchar_t", "wchar_t");
  RegisterTokenBuiltin("short", "short");
  RegisterTokenBuiltin("int", "int");
  RegisterTokenBuiltin("long", "long");
  RegisterTokenBuiltin("long long", "long long");
  RegisterTokenBuiltin("unsigned char", "unsigned char");
  RegisterTokenBuiltin("unsigned short", "unsigned short");
  RegisterTokenBuiltin("unsigned int", "unsigned int");
  RegisterTokenBuiltin("unsigned long", "unsigned long");
  RegisterTokenBuiltin("unsigned long long", "unsigned long long");
  RegisterTokenBuiltin("float", "float");
  RegisterTokenBuiltin("double", "double");
  RegisterTokenBuiltin("long double", "long double");
  RegisterTokenBuiltin("nullptr_t", "nullptr_t");
  RegisterTokenBuiltin("<dependent type>", "dependent");
  RegisterTokenBuiltin("auto", "auto");
  RegisterTokenBuiltin("knrfn", "function");
  RegisterTokenBuiltin("__int128", "__int128");
  RegisterTokenBuiltin("unsigned __int128", "unsigned __int128");
  RegisterTokenBuiltin("SEL", "SEL");
  RegisterTokenBuiltin("id", "id");
  RegisterTokenBuiltin("TypeUnion", "TypeUnion");

  MarkedSource lhs_tycon_builtin;
  auto *lhs_tycon = lhs_tycon_builtin.add_child();
  auto *lookup = lhs_tycon_builtin.add_child();
  lookup->set_kind(MarkedSource::LOOKUP_BY_PARAM);
  lookup->set_lookup_index(1);
  lhs_tycon->set_kind(MarkedSource::IDENTIFIER);
  lhs_tycon->set_pre_text("const ");
  RegisterBuiltin("const", lhs_tycon_builtin);
  lhs_tycon->set_pre_text("volatile ");
  RegisterBuiltin("volatile", lhs_tycon_builtin);
  lhs_tycon->set_pre_text("restrict ");
  RegisterBuiltin("restrict", lhs_tycon_builtin);

  MarkedSource rhs_tycon_builtin;
  lookup = rhs_tycon_builtin.add_child();
  auto *rhs_tycon = rhs_tycon_builtin.add_child();
  lookup->set_kind(MarkedSource::LOOKUP_BY_PARAM);
  lookup->set_lookup_index(1);
  rhs_tycon->set_kind(MarkedSource::IDENTIFIER);
  rhs_tycon->set_pre_text("*");
  RegisterBuiltin("ptr", rhs_tycon_builtin);
  rhs_tycon->set_pre_text("&");
  RegisterBuiltin("lvr", rhs_tycon_builtin);
  rhs_tycon->set_pre_text("&&");
  RegisterBuiltin("rvr", rhs_tycon_builtin);
  rhs_tycon->set_pre_text("[incomplete]");
  RegisterBuiltin("iarr", rhs_tycon_builtin);
  rhs_tycon->set_pre_text("[const]");
  RegisterBuiltin("carr", rhs_tycon_builtin);
  rhs_tycon->set_pre_text("[dependent]");
  RegisterBuiltin("darr", rhs_tycon_builtin);

  MarkedSource function_tycon_builtin;
  auto *return_type = function_tycon_builtin.add_child();
  return_type->set_kind(MarkedSource::LOOKUP_BY_PARAM);
  return_type->set_lookup_index(1);
  auto *args = function_tycon_builtin.add_child();
  args->set_kind(MarkedSource::PARAMETER_LOOKUP_BY_PARAM);
  args->set_pre_text("(");
  args->set_post_child_text(", ");
  args->set_post_text(")");
  args->set_lookup_index(2);
  RegisterBuiltin("fn", function_tycon_builtin);
  auto *vararg_keyword = function_tycon_builtin.add_child();
  vararg_keyword->set_kind(MarkedSource::IDENTIFIER);
  vararg_keyword->set_pre_text("vararg");
  RegisterBuiltin("fnvararg", function_tycon_builtin);
}

void KytheGraphObserver::EmitBuiltin(Builtin *builtin) {
  builtin->emitted = true;
  VNameRef ref(VNameRefFromNodeId(builtin->node_id));
  recorder_->AddProperty(ref, NodeKindID::kBuiltin);
  recorder_->AddMarkedSource(ref, builtin->marked_source);
}

void KytheGraphObserver::EmitMetaNodes() {
  auto EmitNode = [&](const std::string &name,
                      const MarkedSource &marked_source) {
    auto id =
        NodeId::CreateUncompressed(getDefaultClaimToken(), name + "#meta");
    VNameRef ref(VNameRefFromNodeId(id));
    recorder_->AddProperty(ref, NodeKindID::kMeta);
    recorder_->AddMarkedSource(ref, marked_source);
  };
  MarkedSource tapp_signature;
  auto *ctor_lookup = tapp_signature.add_child();
  ctor_lookup->set_kind(MarkedSource::LOOKUP_BY_PARAM);
  ctor_lookup->set_lookup_index(0);
  auto *tapp_body = tapp_signature.add_child();
  tapp_body->set_kind(MarkedSource::PARAMETER_LOOKUP_BY_PARAM_WITH_DEFAULTS);
  tapp_body->set_pre_text("<");
  tapp_body->set_lookup_index(1);
  tapp_body->set_post_child_text(", ");
  tapp_body->set_post_text(">");
  EmitNode("tapp", tapp_signature);
}

void *KytheClaimToken::clazz_ = nullptr;

}  // namespace kythe
