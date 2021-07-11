// Copyright (c) 2019, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'package:analysis_server/lsp_protocol/protocol_generated.dart';
import 'package:analysis_server/lsp_protocol/protocol_special.dart';
import 'package:analysis_server/src/lsp/constants.dart';
import 'package:analysis_server/src/lsp/handlers/handlers.dart';
import 'package:analysis_server/src/lsp/lsp_analysis_server.dart';
import 'package:analysis_server/src/lsp/mapping.dart';
import 'package:analysis_server/src/services/correction/status.dart';
import 'package:analysis_server/src/services/refactoring/refactoring.dart';
import 'package:analysis_server/src/services/refactoring/rename_class_member.dart';
import 'package:analyzer/dart/analysis/results.dart';
import 'package:path/path.dart' as pathLib;

class PrepareRenameHandler
    extends MessageHandler<TextDocumentPositionParams, RangeAndPlaceholder?> {
  PrepareRenameHandler(LspAnalysisServer server) : super(server);
  @override
  Method get handlesMessage => Method.textDocument_prepareRename;

  @override
  LspJsonHandler<TextDocumentPositionParams> get jsonHandler =>
      TextDocumentPositionParams.jsonHandler;

  @override
  Future<ErrorOr<RangeAndPlaceholder?>> handle(
      TextDocumentPositionParams params, CancellationToken token) async {
    if (!isDartDocument(params.textDocument)) {
      return success(null);
    }

    final pos = params.position;
    final path = pathOfDoc(params.textDocument);
    final unit = await path.mapResult(requireResolvedUnit);
    final offset = await unit.mapResult((unit) => toOffset(unit.lineInfo, pos));

    return offset.mapResult((offset) async {
      final node = await server.getNodeAtOffset(path.result, offset);
      final element = server.getElementOfNode(node);
      if (node == null || element == null) {
        return success(null);
      }

      final refactorDetails =
          RenameRefactoring.getElementToRename(node, element);
      if (refactorDetails == null) {
        return success(null);
      }

      final refactoring = RenameRefactoring.create(
          server.refactoringWorkspace, unit.result, refactorDetails.element);
      if (refactoring == null) {
        return success(null);
      }

      // Check the rename is valid here.
      final initStatus = await refactoring.checkInitialConditions();
      if (initStatus.hasFatalError) {
        return error(
            ServerErrorCodes.RenameNotValid, initStatus.problem!.message, null);
      }

      return success(RangeAndPlaceholder(
        range: toRange(
          unit.result.lineInfo,
          // If the offset is set to -1 it means there is no location for the
          // old name. However since we must provide a range for LSP, we'll use
          // a 0-character span at the originally requested location to ensure
          // it's valid.
          refactorDetails.offset == -1 ? offset : refactorDetails.offset,
          refactorDetails.length,
        ),
        placeholder: refactoring.oldName,
      ));
    });
  }
}

class RenameHandler extends MessageHandler<RenameParams, WorkspaceEdit?> {
  RenameHandler(LspAnalysisServer server) : super(server);

  @override
  Method get handlesMessage => Method.textDocument_rename;

  @override
  LspJsonHandler<RenameParams> get jsonHandler => RenameParams.jsonHandler;

  @override
  Future<ErrorOr<WorkspaceEdit?>> handle(
      RenameParams params, CancellationToken token) async {
    if (!isDartDocument(params.textDocument)) {
      return success(null);
    }

    final pos = params.position;
    final textDocument = params.textDocument;
    final path = pathOfDoc(params.textDocument);
    // If the client provided us a version doc identifier, we'll use it to ensure
    // we're not computing a rename for an old document. If not, we'll just assume
    // the version the server had at the time of recieving the request is valid
    // and then use it to verify the document hadn't changed again before we
    // send the edits.
    final docIdentifier = await path.mapResult((path) => success(
        textDocument is OptionalVersionedTextDocumentIdentifier
            ? textDocument
            : textDocument is VersionedTextDocumentIdentifier
                ? OptionalVersionedTextDocumentIdentifier(
                    uri: textDocument.uri, version: textDocument.version)
                : server.getVersionedDocumentIdentifier(path)));

    final unit = await path.mapResult(requireResolvedUnit);
    final offset = await unit.mapResult((unit) => toOffset(unit.lineInfo, pos));

    return offset.mapResult((offset) async {
      final node = await server.getNodeAtOffset(path.result, offset);
      final element = server.getElementOfNode(node);
      if (node == null || element == null) {
        return success(null);
      }

      final refactorDetails =
          RenameRefactoring.getElementToRename(node, element);
      if (refactorDetails == null) {
        return success(null);
      }

      final refactoring = RenameRefactoring.create(
          server.refactoringWorkspace, unit.result, refactorDetails.element);
      if (refactoring == null) {
        return success(null);
      }

      // TODO(dantup): Consider using window/showMessageRequest to prompt
      // the user to see if they'd like to proceed with a rename if there
      // are non-fatal errors or warnings. For now we will reject all errors
      // (fatal and not) as this seems like the most logical behaviour when
      // without a prompt.

      // Check the rename is valid here.
      final renameValidError = _checkRenameValid(refactoring, token);
      if (renameValidError != null) {
        return renameValidError;
      }

      refactoring.newName = params.newName;

      final nameValidError = _checkNameValid(refactoring);
      if (nameValidError != null) {
        return nameValidError;
      }

      final finalValidationError = _finalValidation(refactoring, token);
      if (finalValidationError != null) {
        return finalValidationError;
      }

      // Compute the actual change.
      final change = await refactoring.createChange();
      if (token.isCancellationRequested) {
        return cancelled();
      }

      // Before we send anything back, ensure the original file didn't change
      // while we were computing changes.
      if (fileHasBeenModified(path.result, docIdentifier.result.version)) {
        return fileModifiedError;
      }

      final workspaceEdit = createWorkspaceEdit(server, change);

      bool changeClassName = refactoring is RenameClassMemberRefactoringImpl;

      // TODO: change class name when change file name
      if (!changeClassName) {
        return success(workspaceEdit);
      }

      bool saveToNotChangeFileName =
          server.clientConfiguration.changeClassFileName == false;
      if (saveToNotChangeFileName) {
        return success(workspaceEdit);
      }

      return _renameFile(
        refactoring,
        token,
        workspaceEdit,
        path,
        unit,
        params.newName,
      );
    });
  }

  _checkRenameValid(
      RenameRefactoring refactoring, CancellationToken token) async {
    final initStatus = await refactoring.checkInitialConditions();
    if (token.isCancellationRequested) {
      return cancelled();
    }
    if (initStatus.hasFatalError) {
      return error(
          ServerErrorCodes.RenameNotValid, initStatus.problem!.message, null);
    }
    return null;
  }

  _checkNameValid(RenameRefactoring refactoring) {
    final optionsStatus = refactoring.checkNewName();
    if (optionsStatus.hasError) {
      return error(ServerErrorCodes.RenameNotValid,
          optionsStatus.problem!.message, null);
    }

    return null;
  }

  _finalValidation(
    RenameRefactoring refactoring,
    CancellationToken token,
  ) async {
    final finalStatus = await refactoring.checkFinalConditions();
    if (token.isCancellationRequested) {
      return cancelled();
    }
    if (finalStatus.hasFatalError) {
      return error(
          ServerErrorCodes.RenameNotValid, finalStatus.problem!.message, null);
    } else if (finalStatus.hasError || finalStatus.hasWarning) {
      // Ask the user whether to proceed with the rename.
      final userChoice = await _askUserProceedRename(finalStatus);

      if (token.isCancellationRequested) {
        return cancelled();
      }

      if (userChoice == false) {
        // Return an empty workspace edit response so we do not perform any
        // rename, but also so we do not cause the client to show the user an
        // error after they clicked cancel.
        return success(emptyWorkspaceEdit);
      }
    }

    return null;
  }

  Future<bool> _askUserProceedRename(RefactoringStatus finalStatus) async {
    final userChoice = await server.showUserPrompt(
      MessageType.Warning,
      finalStatus.message!,
      [
        MessageActionItem(title: UserPromptActions.renameAnyway),
        MessageActionItem(title: UserPromptActions.cancel),
      ],
    );
    return userChoice.title == UserPromptActions.renameAnyway;
  }

  Future<ErrorOr<WorkspaceEdit?>> _renameFile(
    RenameRefactoring refactoring,
    CancellationToken token,
    WorkspaceEdit workspaceEdit,
    ErrorOr<String> path,
    ErrorOr<ResolvedUnitResult> unit,
    String newName,
  ) async {
    // if need to change the file name
    final oldClassName = refactoring.oldName;

    final oldPath = path.result;

    final oldPathResource = server.resourceProvider.getResource(oldPath);

    final oldNameFile = oldPathResource.shortName;

    bool needToChangeFileName = _checkSameString(oldClassName, oldNameFile);

    if (!needToChangeFileName) {
      return success(workspaceEdit);
    }

    final newFileName = _toFileName(newName);

    final fileFolder = oldPathResource.parent2;

    final folderPath = fileFolder.path;

    final newFilePath = pathLib.join(folderPath, newFileName);

    if (server.clientConfiguration.changeClassFileName == null) {
      final userChoice = await _askUserIfChangeFileName(
        oldNameFile,
        newFileName,
      );

      if (token.isCancellationRequested) {
        return success(workspaceEdit);
      }

      if (userChoice == false) {
        return success(workspaceEdit);
      }
    }

    //final clientCapabilities = server.clientCapabilities;

    final refactoringFileName = MoveFileRefactoring(server.resourceProvider,
        server.refactoringWorkspace, unit.result, oldPath)
      ..newFile = newFilePath;

    final changeFileName = await refactoringFileName.createChange();

    final edit = createWorkspaceEdit(server, changeFileName);

    return success(edit);
  }

  Future<bool> _askUserIfChangeFileName(
      String oldNameFile, String newFileName) async {
    final userChoice = await server.showUserPrompt(
      MessageType.Info,
      "Do you want also to change the file $oldNameFile name to $newFileName?",
      [
        MessageActionItem(title: UserPromptActions.yes),
        MessageActionItem(title: UserPromptActions.no),
      ],
    );

    return userChoice.title == UserPromptActions.yes;
  }

  bool _checkSameString(
      String upperCamelCase, String lowerCaseWithUnderscores) {
    return _toFileName(upperCamelCase) == lowerCaseWithUnderscores;
  }

  final _upperCasePattern = RegExp('[A-Z]');
  String _toFileName(String className) => className
      .replaceAllMapped(_upperCasePattern,
          (match) => match.start == 0 ? match[0]! : '_${match[0]}')
      .toLowerCase();
}
