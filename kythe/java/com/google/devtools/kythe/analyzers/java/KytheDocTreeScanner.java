/*
 * Copyright 2015 Google Inc. All rights reserved.
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

package com.google.devtools.kythe.analyzers.java;

import com.google.devtools.kythe.analyzers.base.EntrySet;
import com.google.devtools.kythe.common.FormattingLogger;
import com.sun.source.doctree.ReferenceTree;
import com.sun.source.util.DocTreePath;
import com.sun.source.util.DocTreePathScanner;
import com.sun.source.util.DocTrees;
import com.sun.source.util.TreePath;
import com.sun.tools.javac.api.JavacTrees;
import com.sun.tools.javac.code.Symbol;
import com.sun.tools.javac.tree.DCTree.DCDocComment;
import com.sun.tools.javac.tree.DCTree.DCReference;
import com.sun.tools.javac.util.Context;
import java.util.ArrayList;
import java.util.List;

public class KytheDocTreeScanner extends DocTreePathScanner<Void, DCDocComment> {
  private static final FormattingLogger logger =
      FormattingLogger.getLogger(KytheDocTreeScanner.class);

  private final KytheTreeScanner treeScanner;
  private final List<MiniAnchor<Symbol>> miniAnchors;
  private final DocTrees trees;

  public KytheDocTreeScanner(KytheTreeScanner treeScanner, Context context) {
    this.treeScanner = treeScanner;
    this.miniAnchors = new ArrayList<>();
    this.trees = JavacTrees.instance(context);
  }

  public boolean visitDocComment(TreePath treePath, EntrySet node, EntrySet absNode) {
    // TODO(https://phabricator-dot-kythe-repo.appspot.com/T185): always use absNode
    DCDocComment doc = (DCDocComment) trees.getDocCommentTree(treePath);
    if (doc == null) {
      return false;
    }

    miniAnchors.clear();
    scan(new DocTreePath(treePath, doc), doc);
    int startChar = (int) doc.getSourcePosition(doc);

    String bracketed =
        MiniAnchor.bracket(
            doc.comment.getText(),
            new MiniAnchor.PositionTransform() {
              @Override
              public int transform(int pos) {
                return doc.comment.getSourcePos(pos);
              }
            },
            miniAnchors);
    List<Symbol> anchoredTo = new ArrayList<>(miniAnchors.size());
    for (MiniAnchor<Symbol> miniAnchor : miniAnchors) {
      anchoredTo.add(miniAnchor.getAnchoredTo());
    }
    treeScanner.emitDoc(bracketed, anchoredTo, node, absNode);
    return treeScanner.emitCommentsOnLine(treeScanner.charToLine(startChar), node);
  }

  @Override
  public Void visitReference(ReferenceTree tree, DCDocComment doc) {
    DCReference ref = (DCReference) tree;

    Symbol sym = null;
    try {
      sym = (Symbol) trees.getElement(getCurrentPath());
    } catch (Symbol.CompletionFailure e) {
      logger.warningfmt(e, "Failed to resolve documentation reference: %s", tree);
    }
    if (sym == null) {
      return null;
    }

    int startPos = (int) ref.getSourcePosition(doc);
    int endPos = ref.getEndPos(doc);

    treeScanner.emitDocReference(sym, startPos, endPos);
    miniAnchors.add(new MiniAnchor<Symbol>(sym, startPos, endPos));

    return null;
  }
}
