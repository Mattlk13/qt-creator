// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "baseeditordocumentprocessor.h"

#include "cppcodemodelsettings.h"
#include "cppmodelmanager.h"
#include "cpptoolsreuse.h"
#include "editordocumenthandle.h"

#include <projectexplorer/projectmanager.h>

#include <texteditor/quickfix.h>

#include <QPromise>

using namespace Utils;

namespace CppEditor {

/*!
    \class CppEditor::BaseEditorDocumentProcessor

    \brief The BaseEditorDocumentProcessor class controls and executes all
           document relevant actions (reparsing, semantic highlighting, additional
           semantic calculations) after a text document has changed.
*/

BaseEditorDocumentProcessor::BaseEditorDocumentProcessor(QTextDocument *textDocument,
                                                         const FilePath &filePath)
    : m_filePath(filePath),
      m_textDocument(textDocument),
      m_settings(CppCodeModelSettings::settingsForFile(filePath))
{
}

BaseEditorDocumentProcessor::~BaseEditorDocumentProcessor() = default;

void BaseEditorDocumentProcessor::run(bool projectsUpdated)
{
    if (projectsUpdated)
        m_settings = CppCodeModelSettings::settingsForFile(m_filePath);

    const Language languagePreference
        = m_settings.interpretAmbigiousHeadersAsC ? Language::C : Language::Cxx;

    runImpl({CppModelManager::workingCopy(),
             ProjectExplorer::ProjectManager::startupProject(),
             languagePreference,
             projectsUpdated});
}

TextEditor::QuickFixOperations
BaseEditorDocumentProcessor::extraRefactoringOperations(const TextEditor::AssistInterface &)
{
    return TextEditor::QuickFixOperations();
}

void BaseEditorDocumentProcessor::invalidateDiagnostics()
{
}

void BaseEditorDocumentProcessor::setParserConfig(
        const BaseEditorDocumentParser::Configuration &config)
{
    parser()->setConfiguration(config);
}

void BaseEditorDocumentProcessor::runParser(QPromise<void> &promise,
                                            BaseEditorDocumentParser::Ptr parser,
                                            BaseEditorDocumentParser::UpdateParams updateParams)
{
    promise.setProgressRange(0, 1);
    if (promise.isCanceled()) {
        promise.setProgressValue(1);
        return;
    }

    parser->update(promise, updateParams);
    CppModelManager::finishedRefreshingSourceFiles({parser->filePath()});

    promise.setProgressValue(1);
}

} // namespace CppEditor
