// Copyright (c) 2018, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'package:analysis_server/lsp_protocol/protocol_generated.dart';
import 'package:analysis_server/lsp_protocol/protocol_special.dart';
import 'package:analysis_server/plugin/edit/assist/assist_core.dart';
import 'package:analysis_server/plugin/edit/fix/fix_core.dart';
import 'package:analysis_server/src/lsp/constants.dart';
import 'package:analysis_server/src/lsp/handlers/handlers.dart';
import 'package:analysis_server/src/lsp/lsp_analysis_server.dart';
import 'package:analysis_server/src/lsp/mapping.dart';
import 'package:analysis_server/src/protocol_server.dart' hide Position;
import 'package:analysis_server/src/services/correction/assist.dart';
import 'package:analysis_server/src/services/correction/assist_internal.dart';
import 'package:analysis_server/src/services/correction/change_workspace.dart';
import 'package:analysis_server/src/services/correction/fix.dart';
import 'package:analysis_server/src/services/correction/fix/dart/top_level_declarations.dart';
import 'package:analysis_server/src/services/correction/fix_internal.dart';
import 'package:analysis_server/src/services/refactoring/refactoring.dart';
import 'package:analyzer/dart/analysis/results.dart';
import 'package:analyzer/dart/analysis/session.dart'
    show InconsistentAnalysisException;
import 'package:analyzer/error/error.dart';
import 'package:analyzer/src/util/file_paths.dart' as file_paths;
import 'package:collection/collection.dart' show groupBy;

class CodeActionHandler extends MessageHandler<CodeActionParams,
    List<Either2<Command, CodeAction>>> {
  CodeActionHandler(LspAnalysisServer server) : super(server);

  @override
  Method get handlesMessage => Method.textDocument_codeAction;

  @override
  LspJsonHandler<CodeActionParams> get jsonHandler =>
      CodeActionParams.jsonHandler;

  @override
  Future<ErrorOr<List<Either2<Command, CodeAction>>>> handle(
      CodeActionParams params, CancellationToken token) async {
    if (!isDartDocument(params.textDocument)) {
      return success(const []);
    }

    final path = pathOfDoc(params.textDocument);
    if (!path.isError && !server.isAnalyzed(path.result)) {
      return success(const []);
    }

    final clientCapabilities = server.clientCapabilities;
    if (clientCapabilities == null) {
      // This should not happen unless a client misbehaves.
      return error(ErrorCodes.ServerNotInitialized,
          'Requests not before server is initilized');
    }

    final supportsApplyEdit = clientCapabilities.applyEdit;
    final supportsLiteralCodeActions = clientCapabilities.literalCodeActions;
    final supportedKinds = clientCapabilities.codeActionKinds;
    final supportedDiagnosticTags = clientCapabilities.diagnosticTags;

    final unit = await path.mapResult(requireResolvedUnit);

    bool shouldIncludeKind(CodeActionKind? kind) {
      /// Checks whether the kind matches the [wanted] kind.
      ///
      /// If `wanted` is `refactor.foo` then:
      ///  - refactor.foo - included
      ///  - refactor.foobar - not included
      ///  - refactor.foo.bar - included
      bool isMatch(CodeActionKind wanted) =>
          kind == wanted || kind.toString().startsWith('${wanted.toString()}.');

      // If the client wants only a specific set, use only that filter.
      final only = params.context.only;
      if (only != null) {
        return only.any(isMatch);
      }

      // Otherwise, filter out anything not supported by the client (if they
      // advertised that they provided the kinds).
      if (supportsLiteralCodeActions && !supportedKinds.any(isMatch)) {
        return false;
      }

      return true;
    }

    return unit.mapResult((unit) {
      final startOffset = toOffset(unit.lineInfo, params.range.start);
      final endOffset = toOffset(unit.lineInfo, params.range.end);
      return startOffset.mapResult((startOffset) {
        return endOffset.mapResult((endOffset) {
          final offset = startOffset;
          final length = endOffset - startOffset;
          return _getCodeActions(
              shouldIncludeKind,
              supportsLiteralCodeActions,
              supportsApplyEdit,
              supportedDiagnosticTags,
              path.result,
              params.range,
              offset,
              length,
              unit);
        });
      });
    });
  }

  /// Creates a comparer for [CodeActions] that compares the column distance from [pos].
  int Function(CodeAction a, CodeAction b) _codeActionColumnDistanceComparer(
      Position pos) {
    Position posOf(CodeAction action) {
      final diagnostics = action.diagnostics;
      return diagnostics != null && diagnostics.isNotEmpty
          ? diagnostics.first.range.start
          : pos;
    }

    return (a, b) => _columnDistance(posOf(a), pos)
        .compareTo(_columnDistance(posOf(b), pos));
  }

  /// Returns the distance (in columns, ignoring lines) between two positions.
  int _columnDistance(Position a, Position b) =>
      (a.character - b.character).abs();

  /// Wraps a command in a CodeAction if the client supports it so that a
  /// CodeActionKind can be supplied.
  Either2<Command, CodeAction> _commandOrCodeAction(
    bool supportsLiteralCodeActions,
    CodeActionKind kind,
    Command command,
  ) {
    return supportsLiteralCodeActions
        ? Either2<Command, CodeAction>.t2(
            CodeAction(title: command.title, kind: kind, command: command),
          )
        : Either2<Command, CodeAction>.t1(command);
  }

  /// Creates a CodeAction to apply this assist. Note: This code will fetch the
  /// version of each document being modified so it's important to call this
  /// immediately after computing edits to ensure the document is not modified
  /// before the version number is read.
  CodeAction _createAssistAction(Assist assist) {
    return CodeAction(
      title: assist.change.message,
      kind: toCodeActionKind(assist.change.id, CodeActionKind.Refactor),
      diagnostics: const [],
      edit: createWorkspaceEdit(server, assist.change),
    );
  }

  /// Creates a CodeAction to apply this fix. Note: This code will fetch the
  /// version of each document being modified so it's important to call this
  /// immediately after computing edits to ensure the document is not modified
  /// before the version number is read.
  CodeAction _createFixAction(Fix fix, Diagnostic diagnostic) {
    return CodeAction(
      title: fix.change.message,
      kind: toCodeActionKind(fix.change.id, CodeActionKind.QuickFix),
      diagnostics: [diagnostic],
      edit: createWorkspaceEdit(server, fix.change),
    );
  }

  /// Dedupes/merges actions that have the same title, selecting the one nearest [pos].
  ///
  /// If actions perform the same edit/command, their diagnostics will be merged
  /// together. Otherwise, the additional accounts are just dropped.
  ///
  /// The first diagnostic for an action is used to determine the position (using
  /// its `start`). If there is no diagnostic, it will be treated as being at [pos].
  ///
  /// If multiple actions have the same position, one will arbitrarily be chosen.
  List<CodeAction> _dedupeActions(Iterable<CodeAction> actions, Position pos) {
    final groups = groupBy(actions, (CodeAction action) => action.title);
    return groups.entries.map((entry) {
      final actions = entry.value;

      // If there's only one in the group, just return it.
      if (actions.length == 1) {
        return actions.single;
      }

      // Otherwise, find the action nearest to the caret.
      actions.sort(_codeActionColumnDistanceComparer(pos));
      final first = actions.first;

      // Get any actions with the same fix (edit/command) for merging diagnostics.
      final others = actions.skip(1).where(
            (other) =>
                // Compare either edits or commands based on which the selected action has.
                first.edit != null
                    ? first.edit == other.edit
                    : first.command != null
                        ? first.command == other.command
                        : false,
          );

      // Build a new CodeAction that merges the diagnostics from each same
      // code action onto a single one.
      return CodeAction(
        title: first.title,
        kind: first.kind,
        // Merge diagnostics from all of the matching CodeActions.
        diagnostics: [
          ...?first.diagnostics,
          for (final other in others) ...?other.diagnostics,
        ],
        edit: first.edit,
        command: first.command,
      );
    }).toList();
  }

  Future<List<Either2<Command, CodeAction>>> _getAssistActions(
    bool Function(CodeActionKind?) shouldIncludeKind,
    bool supportsLiteralCodeActions,
    Range range,
    int offset,
    int length,
    ResolvedUnitResult unit,
  ) async {
    try {
      var context = DartAssistContextImpl(
        server.instrumentationService,
        DartChangeWorkspace(server.currentSessions),
        unit,
        offset,
        length,
      );
      final processor = AssistProcessor(context);
      final assists = await processor.compute();
      assists.sort(Assist.SORT_BY_RELEVANCE);

      final assistActions =
          _dedupeActions(assists.map(_createAssistAction), range.start);

      return assistActions
          .where((action) => shouldIncludeKind(action.kind))
          .map((action) => Either2<Command, CodeAction>.t2(action))
          .toList();
    } on InconsistentAnalysisException {
      // If an InconsistentAnalysisException occurs, it's likely the user modified
      // the source and therefore is no longer interested in the results, so
      // just return an empty set.
      return [];
    }
  }

  Future<ErrorOr<List<Either2<Command, CodeAction>>>> _getCodeActions(
    bool Function(CodeActionKind?) shouldIncludeKind,
    bool supportsLiterals,
    bool supportsWorkspaceApplyEdit,
    Set<DiagnosticTag> supportedDiagnosticTags,
    String path,
    Range range,
    int offset,
    int length,
    ResolvedUnitResult unit,
  ) async {
    final results = await Future.wait([
      _getSourceActions(shouldIncludeKind, supportsLiterals,
          supportsWorkspaceApplyEdit, path),
      _getAssistActions(
          shouldIncludeKind, supportsLiterals, range, offset, length, unit),
      _getRefactorActions(
          shouldIncludeKind, supportsLiterals, path, offset, length, unit),
      _getFixActions(shouldIncludeKind, supportsLiterals,
          supportedDiagnosticTags, range, unit),
    ]);
    final flatResults = results.expand((x) => x).toList();

    return success(flatResults);
  }

  Future<List<Either2<Command, CodeAction>>> _getFixActions(
    bool Function(CodeActionKind?) shouldIncludeKind,
    bool supportsLiteralCodeActions,
    Set<DiagnosticTag> supportedDiagnosticTags,
    Range range,
    ResolvedUnitResult unit,
  ) async {
    final lineInfo = unit.lineInfo;
    final codeActions = <CodeAction>[];
    final fixContributor = DartFixContributor();

    try {
      final errorCodeCounts = <ErrorCode, int>{};
      // Count the errors by code so we know whether to include a fix-all.
      for (final error in unit.errors) {
        errorCodeCounts[error.errorCode] =
            (errorCodeCounts[error.errorCode] ?? 0) + 1;
      }

      for (final error in unit.errors) {
        // Server lineNumber is one-based so subtract one.
        var errorLine = lineInfo.getLocation(error.offset).lineNumber - 1;
        if (errorLine < range.start.line || errorLine > range.end.line) {
          continue;
        }
        var workspace = DartChangeWorkspace(server.currentSessions);
        var context = DartFixContextImpl(
            server.instrumentationService, workspace, unit, error, (name) {
          var tracker = server.declarationsTracker!;
          return TopLevelDeclarationsProvider(tracker).get(
            unit.session.analysisContext,
            unit.path!,
            name,
          );
        }, extensionCache: server.getExtensionCacheFor(unit));
        final fixes = await fixContributor.computeFixes(context);
        if (fixes.isNotEmpty) {
          fixes.sort(Fix.SORT_BY_RELEVANCE);

          final diagnostic = toDiagnostic(
            unit,
            error,
            supportedTags: supportedDiagnosticTags,
            clientSupportsCodeDescription:
                server.clientCapabilities?.diagnosticCodeDescription ?? false,
          );
          codeActions.addAll(
            fixes.map((fix) => _createFixAction(fix, diagnostic)),
          );
        }
      }

      final dedupedActions = _dedupeActions(codeActions, range.start);

      return dedupedActions
          .where((action) => shouldIncludeKind(action.kind))
          .map((action) => Either2<Command, CodeAction>.t2(action))
          .toList();
    } on InconsistentAnalysisException {
      // If an InconsistentAnalysisException occurs, it's likely the user modified
      // the source and therefore is no longer interested in the results, so
      // just return an empty set.
      return [];
    }
  }

  Future<List<Either2<Command, CodeAction>>> _getRefactorActions(
    bool Function(CodeActionKind) shouldIncludeKind,
    bool supportsLiteralCodeActions,
    String path,
    int offset,
    int length,
    ResolvedUnitResult unit,
  ) async {
    // The refactor actions supported are only valid for Dart files.
    var pathContext = server.resourceProvider.pathContext;
    if (!file_paths.isDart(pathContext, path)) {
      return const [];
    }

    /// Helper to create refactors that execute commands provided with
    /// the current file, location and document version.
    Either2<Command, CodeAction> createRefactor(
      CodeActionKind actionKind,
      String name,
      RefactoringKind refactorKind, [
      Map<String, dynamic>? options,
    ]) {
      return _commandOrCodeAction(
          supportsLiteralCodeActions,
          actionKind,
          Command(
            title: name,
            command: Commands.performRefactor,
            arguments: [
              refactorKind.toJson(),
              path,
              server.getVersionedDocumentIdentifier(path).version,
              offset,
              length,
              options,
            ],
          ));
    }

    try {
      final refactorActions = <Either2<Command, CodeAction>>[];

      // Extracts
      if (shouldIncludeKind(CodeActionKind.RefactorExtract)) {
        // Extract Method
        if (ExtractMethodRefactoring(server.searchEngine, unit, offset, length)
            .isAvailable()) {
          refactorActions.add(createRefactor(CodeActionKind.RefactorExtract,
              'Extract Method', RefactoringKind.EXTRACT_METHOD));
        }

        // Extract Local Variable
        if (ExtractLocalRefactoring(unit, offset, length).isAvailable()) {
          refactorActions.add(createRefactor(
              CodeActionKind.RefactorExtract,
              'Extract Local Variable',
              RefactoringKind.EXTRACT_LOCAL_VARIABLE));
        }

        // Extract Widget
        if (ExtractWidgetRefactoring(server.searchEngine, unit, offset, length)
            .isAvailable()) {
          refactorActions.add(createRefactor(CodeActionKind.RefactorExtract,
              'Extract Widget', RefactoringKind.EXTRACT_WIDGET));
        }
      }

      // Inlines
      if (shouldIncludeKind(CodeActionKind.RefactorInline)) {
        // Inline Local Variable
        if (InlineLocalRefactoring(server.searchEngine, unit, offset)
            .isAvailable()) {
          refactorActions.add(createRefactor(CodeActionKind.RefactorInline,
              'Inline Local Variable', RefactoringKind.INLINE_LOCAL_VARIABLE));
        }

        // Inline Method
        if (InlineMethodRefactoring(server.searchEngine, unit, offset)
            .isAvailable()) {
          refactorActions.add(createRefactor(CodeActionKind.RefactorInline,
              'Inline Method', RefactoringKind.INLINE_METHOD));
        }
      }

      return refactorActions;
    } on InconsistentAnalysisException {
      // If an InconsistentAnalysisException occurs, it's likely the user modified
      // the source and therefore is no longer interested in the results, so
      // just return an empty set.
      return [];
    }
  }

  /// Gets "Source" CodeActions, which are actions that apply to whole files of
  /// source such as Sort Members and Organise Imports.
  Future<List<Either2<Command, CodeAction>>> _getSourceActions(
    bool Function(CodeActionKind) shouldIncludeKind,
    bool supportsLiteralCodeActions,
    bool supportsApplyEdit,
    String path,
  ) async {
    // The source actions supported are only valid for Dart files.
    var pathContext = server.resourceProvider.pathContext;
    if (!file_paths.isDart(pathContext, path)) {
      return const [];
    }

    // If the client does not support workspace/applyEdit, we won't be able to
    // run any of these.
    if (!supportsApplyEdit) {
      return const [];
    }

    return [
      if (shouldIncludeKind(DartCodeActionKind.SortMembers))
        _commandOrCodeAction(
          supportsLiteralCodeActions,
          DartCodeActionKind.SortMembers,
          Command(
              title: 'Sort Members',
              command: Commands.sortMembers,
              arguments: [path]),
        ),
      if (shouldIncludeKind(CodeActionKind.SourceOrganizeImports))
        _commandOrCodeAction(
          supportsLiteralCodeActions,
          CodeActionKind.SourceOrganizeImports,
          Command(
              title: 'Organize Imports',
              command: Commands.organizeImports,
              arguments: [path]),
        ),
      if (shouldIncludeKind(DartCodeActionKind.FixAll))
        _commandOrCodeAction(
          supportsLiteralCodeActions,
          DartCodeActionKind.FixAll,
          Command(
              title: 'Fix All', command: Commands.fixAll, arguments: [path]),
        ),
    ];
  }
}
