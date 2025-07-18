// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "cppfunctiondecldeflink.h"

#include "cppcodestylesettings.h"
#include "cppeditorconstants.h"
#include "cppeditortr.h"
#include "cppeditorwidget.h"
#include "cpplocalsymbols.h"
#include "cpptoolsreuse.h"
#include "quickfixes/cppquickfixassistant.h"
#include "symbolfinder.h"

#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/actionmanager/command.h>

#include <cplusplus/ASTPath.h>
#include <cplusplus/CppRewriter.h>
#include <cplusplus/declarationcomments.h>
#include <cplusplus/Overview.h>
#include <cplusplus/TypeOfExpression.h>

#include <texteditor/refactoroverlay.h>
#include <texteditor/texteditorconstants.h>

#include <utils/async.h>
#include <utils/futuresynchronizer.h>
#include <utils/proxyaction.h>
#include <utils/qtcassert.h>
#include <utils/textutils.h>
#include <utils/tooltip/tooltip.h>

#include <QRegularExpression>
#include <QVarLengthArray>

using namespace CPlusPlus;
using namespace TextEditor;
using namespace Utils;

namespace CppEditor {
namespace Internal {

FunctionDeclDefLinkFinder::FunctionDeclDefLinkFinder(QObject *parent)
    : QObject(parent)
{}

void FunctionDeclDefLinkFinder::onFutureDone()
{
    std::shared_ptr<FunctionDeclDefLink> link = m_watcher->result();
    m_watcher.release()->deleteLater();
    if (link) {
        link->linkSelection = m_scannedSelection;
        link->nameSelection = m_nameSelection;
        if (m_nameSelection.selectedText() != link->nameInitial)
            link.reset();
    }
    m_scannedSelection = {};
    m_nameSelection = {};
    if (link)
        emit foundLink(link);
}

QTextCursor FunctionDeclDefLinkFinder::scannedSelection() const
{
    return m_scannedSelection;
}

// parent is either a FunctionDefinitionAST or a SimpleDeclarationAST
// line and column are 1-based
static bool findDeclOrDef(const Document::Ptr &doc, int line, int column,
                          DeclarationAST **parent, DeclaratorAST **decl,
                          FunctionDeclaratorAST **funcDecl)
{
    QList<AST *> path = ASTPath(doc)(line, column);

    // for function definitions, simply scan for FunctionDefinitionAST not preceded
    //    by CompoundStatement/CtorInitializer
    // for function declarations, look for SimpleDeclarations with a single Declarator
    //    with a FunctionDeclarator postfix
    *decl = nullptr;
    for (int i = path.size() - 1; i > 0; --i) {
        AST *ast = path.at(i);
        if (ast->asCompoundStatement() || ast->asCtorInitializer())
            break;
        if (FunctionDefinitionAST *funcDef = ast->asFunctionDefinition()) {
            *parent = funcDef;
            *decl = funcDef->declarator;
            break;
        }
        if (SimpleDeclarationAST *simpleDecl = ast->asSimpleDeclaration()) {
            *parent = simpleDecl;
            if (!simpleDecl->declarator_list || !simpleDecl->declarator_list->value)
                break;
            *decl = simpleDecl->declarator_list->value;
            break;
        }
    }
    if (!*parent || !*decl)
        return false;
    if (!(*decl)->postfix_declarator_list || !(*decl)->postfix_declarator_list->value)
        return false;
    *funcDecl = (*decl)->postfix_declarator_list->value->asFunctionDeclarator();
    return *funcDecl;
}

static void declDefLinkStartEnd(const CppRefactoringFileConstPtr &file,
                                DeclarationAST *parent, FunctionDeclaratorAST *funcDecl,
                                int *start, int *end)
{
    *start = file->startOf(parent);
    if (funcDecl->trailing_return_type)
        *end = file->endOf(funcDecl->trailing_return_type);
    else if (funcDecl->exception_specification)
        *end = file->endOf(funcDecl->exception_specification);
    else if (funcDecl->cv_qualifier_list)
        *end = file->endOf(funcDecl->cv_qualifier_list->lastValue());
    else
        *end = file->endOf(funcDecl->rparen_token);
}

static DeclaratorIdAST *getDeclaratorId(DeclaratorAST *declarator)
{
    if (!declarator || !declarator->core_declarator)
        return nullptr;
    if (DeclaratorIdAST *id = declarator->core_declarator->asDeclaratorId())
        return id;
    if (NestedDeclaratorAST *nested = declarator->core_declarator->asNestedDeclarator())
        return getDeclaratorId(nested->declarator);
    return nullptr;
}

static std::shared_ptr<FunctionDeclDefLink> findLinkHelper(std::shared_ptr<FunctionDeclDefLink> link, CppRefactoringChanges changes)
{
    std::shared_ptr<FunctionDeclDefLink> noResult;
    const Snapshot &snapshot = changes.snapshot();

    // find the matching decl/def symbol
    Symbol *target = nullptr;
    SymbolFinder finder;
    if (FunctionDefinitionAST *funcDef = link->sourceDeclaration->asFunctionDefinition()) {
        QList<Declaration *> nameMatch, argumentCountMatch, typeMatch;
        finder.findMatchingDeclaration(LookupContext(link->sourceDocument, snapshot),
                                       funcDef->symbol,
                                       &typeMatch, &argumentCountMatch, &nameMatch);
        if (!typeMatch.isEmpty())
            target = typeMatch.first();
    } else if (link->sourceDeclaration->asSimpleDeclaration()) {
        target = finder.findMatchingDefinition(link->sourceFunctionDeclarator->symbol, snapshot, true);
    }
    if (!target)
        return noResult;

    // parse the target file to get the linked decl/def
    CppRefactoringFileConstPtr targetFile = changes.fileNoEditor(target->filePath());
    if (!targetFile->isValid())
        return noResult;

    DeclarationAST *targetParent = nullptr;
    FunctionDeclaratorAST *targetFuncDecl = nullptr;
    DeclaratorAST *targetDeclarator = nullptr;
    if (!findDeclOrDef(targetFile->cppDocument(), target->line(), target->column(),
                       &targetParent, &targetDeclarator, &targetFuncDecl))
        return noResult;

    // the parens are necessary for finding good places for changes
    if (!targetFuncDecl->lparen_token || !targetFuncDecl->rparen_token)
        return noResult;
    QTC_ASSERT(targetFuncDecl->symbol, return noResult);
    // if the source and target argument counts differ, something is wrong
    QTC_ASSERT(targetFuncDecl->symbol->argumentCount() == link->sourceFunction->argumentCount(), return noResult);

    int targetStart, targetEnd;
    declDefLinkStartEnd(targetFile, targetParent, targetFuncDecl, &targetStart, &targetEnd);
    QString targetInitial = targetFile->textOf(
                targetFile->startOf(targetParent),
                targetEnd);

    targetFile->lineAndColumn(targetStart, &link->targetLine, &link->targetColumn);
    link->targetInitial = targetInitial;

    link->targetFile = targetFile;
    link->targetFunction = targetFuncDecl->symbol;
    link->targetFunctionDeclarator = targetFuncDecl;
    link->targetDeclaration = targetParent;

    return link;
}

void FunctionDeclDefLinkFinder::startFindLinkAt(
        QTextCursor cursor, const Document::Ptr &doc, const Snapshot &snapshot)
{
    // check if cursor is on function decl/def
    DeclarationAST *parent = nullptr;
    FunctionDeclaratorAST *funcDecl = nullptr;
    DeclaratorAST *declarator = nullptr;
    if (!findDeclOrDef(doc, cursor.blockNumber() + 1, cursor.columnNumber() + 1,
                       &parent, &declarator, &funcDecl))
        return;

    // find the start/end offsets
    CppRefactoringChanges refactoringChanges(snapshot);
    CppRefactoringFilePtr sourceFile = refactoringChanges.cppFile(doc->filePath());
    sourceFile->setCppDocument(doc);
    int start, end;
    declDefLinkStartEnd(sourceFile, parent, funcDecl, &start, &end);

    // if already scanning, don't scan again
    if (!m_scannedSelection.isNull()
            && m_scannedSelection.selectionStart() == start
            && m_scannedSelection.selectionEnd() == end) {
        return;
    }

    // build the selection for the currently scanned area
    m_scannedSelection = cursor;
    m_scannedSelection.setPosition(end);
    m_scannedSelection.setPosition(start, QTextCursor::KeepAnchor);
    m_scannedSelection.setKeepPositionOnInsert(true);

    // build selection for the name
    DeclaratorIdAST *declId = getDeclaratorId(declarator);
    m_nameSelection = cursor;
    m_nameSelection.setPosition(sourceFile->endOf(declId));
    m_nameSelection.setPosition(sourceFile->startOf(declId), QTextCursor::KeepAnchor);
    m_nameSelection.setKeepPositionOnInsert(true);

    // set up a base result
    std::shared_ptr<FunctionDeclDefLink> result(new FunctionDeclDefLink);
    result->nameInitial = m_nameSelection.selectedText();
    result->sourceDocument = doc;
    result->sourceFunction = funcDecl->symbol;
    result->sourceDeclaration = parent;
    result->sourceFunctionDeclarator = funcDecl;

    // handle the rest in a thread
    m_watcher.reset(new QFutureWatcher<std::shared_ptr<FunctionDeclDefLink> >());
    connect(m_watcher.get(), &QFutureWatcherBase::finished, this, &FunctionDeclDefLinkFinder::onFutureDone);
    m_watcher->setFuture(Utils::asyncRun(findLinkHelper, result, refactoringChanges));
    Utils::futureSynchronizer()->addFuture(m_watcher->future());
}

bool FunctionDeclDefLink::isValid() const
{
    return !linkSelection.isNull();
}

bool FunctionDeclDefLink::isMarkerVisible() const
{
    return hasMarker;
}

static bool namesEqual(const Name *n1, const Name *n2)
{
    return n1 == n2 || (n1 && n2 && n1->match(n2));
}

void FunctionDeclDefLink::apply(CppEditorWidget *editor, bool jumpToMatch)
{
    Snapshot snapshot = editor->semanticInfo().snapshot;

    // first verify the interesting region of the target file is unchanged
    CppRefactoringChanges refactoringChanges(snapshot);
    CppRefactoringFilePtr newTargetFile = refactoringChanges.cppFile(targetFile->filePath());
    if (!newTargetFile->isValid())
        return;
    const int targetStart = newTargetFile->position(targetLine, targetColumn);
    const int targetEnd = targetStart + targetInitial.size();
    if (targetInitial == newTargetFile->textOf(targetStart, targetEnd)) {
        if (jumpToMatch) {
            const int jumpTarget = newTargetFile->position(targetFunction->line(), targetFunction->column());
            newTargetFile->setOpenEditor(true, jumpTarget);
        }
        ChangeSet changeSet = changes(snapshot, targetStart);
        for (ChangeSet::EditOp &op : changeSet.operationList()) {
            if (op.type() == ChangeSet::EditOp::Replace)
                op.setFormat1(true);
        }
        newTargetFile->apply(changeSet);
    } else {
        ToolTip::show(editor->toolTipPosition(linkSelection),
                      Tr::tr("Target file was changed, could not apply changes"));
    }
}

void FunctionDeclDefLink::hideMarker(CppEditorWidget *editor)
{
    if (!hasMarker)
        return;
    editor->clearRefactorMarkers(Constants::CPP_FUNCTION_DECL_DEF_LINK_MARKER_ID);
    hasMarker = false;
}

void FunctionDeclDefLink::showMarker(CppEditorWidget *editor)
{
    if (hasMarker)
        return;

    RefactorMarkers markers;
    RefactorMarker marker;

    // show the marker at the end of the linked area, with a special case
    // to avoid it overlapping with a trailing semicolon
    marker.cursor = editor->textCursor();
    marker.cursor.setPosition(linkSelection.selectionEnd());
    const int endBlockNr = marker.cursor.blockNumber();
    marker.cursor.setPosition(linkSelection.selectionEnd() + 1, QTextCursor::KeepAnchor);
    if (marker.cursor.blockNumber() != endBlockNr
            || marker.cursor.selectedText() != QLatin1String(";")) {
        marker.cursor.setPosition(linkSelection.selectionEnd());
    }

    QString message;
    if (targetDeclaration->asFunctionDefinition())
        message = Tr::tr("Apply changes to definition");
    else
        message = Tr::tr("Apply changes to declaration");

    Core::Command *quickfixCommand = Core::ActionManager::command(TextEditor::Constants::QUICKFIX_THIS);
    if (quickfixCommand)
        message = ProxyAction::stringWithAppendedShortcut(message, quickfixCommand->keySequence());

    marker.tooltip = message;
    marker.type = Constants::CPP_FUNCTION_DECL_DEF_LINK_MARKER_ID;
    marker.callback = [](TextEditor::TextEditorWidget *widget) {
        if (auto cppEditor = qobject_cast<CppEditorWidget *>(widget))
            cppEditor->applyDeclDefLinkChanges(true);
    };
    markers += marker;
    editor->setRefactorMarkers(markers, Constants::CPP_FUNCTION_DECL_DEF_LINK_MARKER_ID);

    hasMarker = true;
}

// does consider foo(void) to have one argument
static int declaredParameterCount(Function *function)
{
    int argc = function->argumentCount();
    if (argc == 0 && function->memberCount() > 0 && function->memberAt(0)->type().type()->asVoidType())
        return 1;
    return argc;
}

Q_GLOBAL_STATIC(QRegularExpression, commentArgNameRegexp)

static bool hasCommentedName(
        TranslationUnit *unit,
        const QString &source,
        FunctionDeclaratorAST *declarator,
        int i)
{
    if (!declarator
            || !declarator->parameter_declaration_clause
            || !declarator->parameter_declaration_clause->parameter_declaration_list)
        return false;

    if (Function *f = declarator->symbol) {
        QTC_ASSERT(f, return false);
        if (Symbol *a = f->argumentAt(i)) {
            QTC_ASSERT(a, return false);
            if (a->name())
                return false;
        }
    }

    ParameterDeclarationListAST *list = declarator->parameter_declaration_clause->parameter_declaration_list;
    while (list && i) {
        list = list->next;
        --i;
    }
    if (!list || !list->value || i)
        return false;

    ParameterDeclarationAST *param = list->value;
    if (param->symbol && param->symbol->name())
        return false;

    // maybe in a comment but in the right spot?
    int nameStart = 0;
    if (param->declarator)
        nameStart = unit->tokenAt(param->declarator->lastToken() - 1).utf16charsEnd();
    else if (param->type_specifier_list)
        nameStart = unit->tokenAt(param->type_specifier_list->lastToken() - 1).utf16charsEnd();
    else
        nameStart = unit->tokenAt(param->firstToken()).utf16charsBegin();

    int nameEnd = 0;
    if (param->equal_token)
        nameEnd = unit->tokenAt(param->equal_token).utf16charsBegin();
    else
        nameEnd = unit->tokenAt(param->lastToken()).utf16charsBegin(); // one token after

    QString text = source.mid(nameStart, nameEnd - nameStart);

    if (commentArgNameRegexp()->pattern().isEmpty())
        *commentArgNameRegexp() = QRegularExpression(QLatin1String("/\\*\\s*(\\w*)\\s*\\*/"));
    return text.indexOf(*commentArgNameRegexp()) != -1;
}

static bool canReplaceSpecifier(TranslationUnit *translationUnit, SpecifierAST *specifier)
{
    if (SimpleSpecifierAST *simple = specifier->asSimpleSpecifier()) {
        switch (translationUnit->tokenAt(simple->specifier_token).kind()) {
        case T_CONST:
        case T_VOLATILE:
        case T_CHAR:
        case T_CHAR16_T:
        case T_CHAR32_T:
        case T_WCHAR_T:
        case T_BOOL:
        case T_SHORT:
        case T_INT:
        case T_LONG:
        case T_SIGNED:
        case T_UNSIGNED:
        case T_FLOAT:
        case T_DOUBLE:
        case T_VOID:
        case T_AUTO:
        case T___TYPEOF__:
        case T___ATTRIBUTE__:
        case T___DECLSPEC:
            return true;
        default:
            return false;
        }
    }
    return !specifier->asAttributeSpecifier();
}

static SpecifierAST *findFirstReplaceableSpecifier(TranslationUnit *translationUnit, SpecifierListAST *list)
{
    for (SpecifierListAST *it = list; it; it = it->next) {
        if (canReplaceSpecifier(translationUnit, it->value))
            return it->value;
    }
    return nullptr;
}

using IndicesList = QVarLengthArray<int, 10>;

template <class IndicesListType>
static int findUniqueTypeMatch(int sourceParamIndex, Function *sourceFunction, Function *newFunction,
                               const IndicesListType &sourceParams, const IndicesListType &newParams)
{
    Symbol *sourceParam = sourceFunction->argumentAt(sourceParamIndex);

    // if other sourceParams have the same type, we can't do anything
    for (int i = 0; i < sourceParams.size(); ++i) {
        int otherSourceParamIndex = sourceParams.at(i);
        if (sourceParamIndex == otherSourceParamIndex)
            continue;
        if (sourceParam->type().match(sourceFunction->argumentAt(otherSourceParamIndex)->type()))
            return -1;
    }

    // if there's exactly one newParam with the same type, bind to that
    // this is primarily done to catch moves of unnamed parameters
    int newParamWithSameTypeIndex = -1;
    for (int i = 0; i < newParams.size(); ++i) {
        int newParamIndex = newParams.at(i);
        if (sourceParam->type().match(newFunction->argumentAt(newParamIndex)->type())) {
            if (newParamWithSameTypeIndex != -1)
                return -1;
            newParamWithSameTypeIndex = newParamIndex;
        }
    }
    return newParamWithSameTypeIndex;
}

static IndicesList unmatchedIndices(const IndicesList &indices)
{
    IndicesList ret;
    ret.reserve(indices.size());
    for (int i = 0; i < indices.size(); ++i) {
        if (indices[i] == -1)
            ret.append(i);
    }
    return ret;
}

static QString ensureCorrectParameterSpacing(const QString &text, bool isFirstParam)
{
    if (isFirstParam) { // drop leading spaces
        int newlineCount = 0;
        int firstNonSpace = 0;
        while (firstNonSpace + 1 < text.size() && text.at(firstNonSpace).isSpace()) {
            if (text.at(firstNonSpace) == QChar::ParagraphSeparator)
                ++newlineCount;
            ++firstNonSpace;
        }
        return QString(newlineCount, QChar::ParagraphSeparator) + text.mid(firstNonSpace);
    } else { // ensure one leading space
        if (text.isEmpty() || !text.at(0).isSpace())
            return QLatin1Char(' ') + text;
    }
    return text;
}

static unsigned findCommaTokenBetween(const CppRefactoringFileConstPtr &file,
                                      ParameterDeclarationAST *left, ParameterDeclarationAST *right)
{
    unsigned last = left->lastToken() - 1;
    for (unsigned tokenIndex = right->firstToken();
         tokenIndex > last;
         --tokenIndex) {
        if (file->tokenAt(tokenIndex).kind() == T_COMMA)
            return tokenIndex;
    }
    return 0;
}

ChangeSet FunctionDeclDefLink::changes(const Snapshot &snapshot, int targetOffset)
{
    ChangeSet changes;

    // Everything prefixed with 'new' in this function relates to the state of the 'source'
    // function *after* the user did his changes.

    // The 'newTarget' prefix indicates something relates to the changes we plan to do
    // to the 'target' function.

    // parse the current source declaration
    TypeOfExpression typeOfExpression; // ### just need to preprocess...
    typeOfExpression.init(sourceDocument, snapshot);

    QString newDeclText = linkSelection.selectedText();
    for (int i = 0; i < newDeclText.size(); ++i) {
        if (newDeclText.at(i).toLatin1() == 0)
            newDeclText[i] = QLatin1Char('\n');
    }
    newDeclText.append(QLatin1String("{}"));
    const QByteArray newDeclTextPreprocessed = typeOfExpression.preprocess(newDeclText.toUtf8());

    Document::Ptr newDeclDoc = Document::create(FilePath::fromPathPart(u"<decl>"));
    newDeclDoc->setUtf8Source(newDeclTextPreprocessed);
    newDeclDoc->parse(Document::ParseDeclaration);
    newDeclDoc->check();

    // extract the function symbol
    if (!newDeclDoc->translationUnit()->ast())
        return changes;
    FunctionDefinitionAST *newDef = newDeclDoc->translationUnit()->ast()->asFunctionDefinition();
    if (!newDef)
        return changes;
    Function *newFunction = newDef->symbol;
    if (!newFunction)
        return changes;

    const Overview overviewFromCurrentProjectStyle
        = CppCodeStyleSettings::currentProjectCodeStyleOverview();

    Overview overview = overviewFromCurrentProjectStyle;
    overview.showReturnTypes = true;
    overview.showTemplateParameters = true;
    overview.showArgumentNames = true;
    overview.showFunctionSignatures = true;

    // abort if the name of the newly parsed function is not the expected one
    DeclaratorIdAST *newDeclId = getDeclaratorId(newDef->declarator);
    if (!newDeclId || !newDeclId->name || !newDeclId->name->name
            || overview.prettyName(newDeclId->name->name) != normalizedInitialName()) {
        return changes;
    }

    LookupContext sourceContext(sourceDocument, snapshot);
    LookupContext targetContext(targetFile->cppDocument(), snapshot);

    // sync return type
    do {
        // set up for rewriting return type
        SubstitutionEnvironment env;
        env.setContext(sourceContext);
        env.switchScope(sourceFunction->enclosingScope());
        ClassOrNamespace *targetCoN = targetContext.lookupType(targetFunction->enclosingScope());
        if (!targetCoN)
            targetCoN = targetContext.globalNamespace();
        UseMinimalNames q(targetCoN);
        env.enter(&q);
        Control *control = sourceContext.bindings()->control().get();

        // get return type start position and declarator info from declaration
        DeclaratorAST *declarator = nullptr;
        SpecifierAST *firstReplaceableSpecifier = nullptr;
        TranslationUnit *targetTranslationUnit = targetFile->cppDocument()->translationUnit();
        if (SimpleDeclarationAST *simple = targetDeclaration->asSimpleDeclaration()) {
            declarator = simple->declarator_list->value;
            firstReplaceableSpecifier = findFirstReplaceableSpecifier(
                        targetTranslationUnit, simple->decl_specifier_list);
        } else if (FunctionDefinitionAST *def = targetDeclaration->asFunctionDefinition()) {
            declarator = def->declarator;
            firstReplaceableSpecifier = findFirstReplaceableSpecifier(
                        targetTranslationUnit, def->decl_specifier_list);
        } else {
            // no proper AST to synchronize the return type
            break;
        }

        int returnTypeStart = 0;
        if (firstReplaceableSpecifier)
            returnTypeStart = targetFile->startOf(firstReplaceableSpecifier);
        else
            returnTypeStart = targetFile->startOf(declarator);

        if (!newFunction->returnType().match(sourceFunction->returnType())
                && !newFunction->returnType().match(targetFunction->returnType())) {
            FullySpecifiedType type = rewriteType(newFunction->returnType(), &env, control);
            const QString replacement = overview.prettyType(type, targetFunction->name());
            changes.replace(returnTypeStart,
                            targetFile->startOf(targetFunctionDeclarator->lparen_token),
                            replacement);
        }
    } while (false);

    // sync parameters
    {
        // set up for rewriting parameter types
        SubstitutionEnvironment env;
        env.setContext(sourceContext);
        env.switchScope(sourceFunction);
        ClassOrNamespace *targetCoN = targetContext.lookupType(targetFunction);
        if (!targetCoN)
            targetCoN = targetContext.globalNamespace();
        UseMinimalNames q(targetCoN);
        env.enter(&q);
        Control *control = sourceContext.bindings()->control().get();
        Overview overview = overviewFromCurrentProjectStyle;
        overview.showReturnTypes = true;
        overview.showTemplateParameters = true;

        // make a easy to access list of the target parameter declarations
        QVarLengthArray<ParameterDeclarationAST *, 10> targetParameterDecls;
        // there is no parameter declaration clause if the function has no arguments
        if (targetFunctionDeclarator->parameter_declaration_clause) {
            for (ParameterDeclarationListAST *it = targetFunctionDeclarator->parameter_declaration_clause->parameter_declaration_list;
                 it; it = it->next) {
                targetParameterDecls.append(it->value);
            }
        }

        // the number of parameters in sourceFunction or targetFunction
        const int existingParamCount = declaredParameterCount(sourceFunction);
        if (existingParamCount != declaredParameterCount(targetFunction))
            return changes;
        if (existingParamCount != targetParameterDecls.size())
            return changes;

        const int newParamCount = declaredParameterCount(newFunction);

        // When syncing parameters we need to take care that parameters inserted or
        // removed in the middle or parameters being reshuffled are treated correctly.
        // To do that, we construct a newParam -> sourceParam map, based on parameter
        // names and types.
        // Initially they start out with -1 to indicate a new parameter.
        IndicesList newParamToSourceParam(newParamCount);
        for (int i = 0; i < newParamCount; ++i)
            newParamToSourceParam[i] = -1;

        // fill newParamToSourceParam
        {
            IndicesList sourceParamToNewParam(existingParamCount);
            for (int i = 0; i < existingParamCount; ++i)
                sourceParamToNewParam[i] = -1;

            QMultiHash<QString, int> sourceParamNameToIndex;
            for (int i = 0; i < existingParamCount; ++i) {
                Symbol *sourceParam = sourceFunction->argumentAt(i);
                sourceParamNameToIndex.insert(overview.prettyName(sourceParam->name()), i);
            }

            QMultiHash<QString, int> newParamNameToIndex;
            for (int i = 0; i < newParamCount; ++i) {
                Symbol *newParam = newFunction->argumentAt(i);
                newParamNameToIndex.insert(overview.prettyName(newParam->name()), i);
            }

            // name-based binds (possibly disambiguated by type)
            for (int sourceParamIndex = 0; sourceParamIndex < existingParamCount; ++sourceParamIndex) {
                Symbol *sourceParam = sourceFunction->argumentAt(sourceParamIndex);
                const QString &name = overview.prettyName(sourceParam->name());
                QList<int> newParams = newParamNameToIndex.values(name);
                QList<int> sourceParams = sourceParamNameToIndex.values(name);

                if (newParams.isEmpty())
                    continue;

                // if the names match uniquely, bind them
                // this catches moves of named parameters
                if (newParams.size() == 1 && sourceParams.size() == 1) {
                    sourceParamToNewParam[sourceParamIndex] = newParams.first();
                    newParamToSourceParam[newParams.first()] = sourceParamIndex;
                } else {
                    // if the name match is not unique, try to find a unique
                    // type match among the same-name parameters
                    const int newParamWithSameTypeIndex = findUniqueTypeMatch(
                                sourceParamIndex, sourceFunction, newFunction,
                                sourceParams, newParams);
                    if (newParamWithSameTypeIndex != -1) {
                        sourceParamToNewParam[sourceParamIndex] = newParamWithSameTypeIndex;
                        newParamToSourceParam[newParamWithSameTypeIndex] = sourceParamIndex;
                    }
                }
            }

            // find unique type matches among the unbound parameters
            const IndicesList &freeSourceParams = unmatchedIndices(sourceParamToNewParam);
            const IndicesList &freeNewParams = unmatchedIndices(newParamToSourceParam);
            for (int i = 0; i < freeSourceParams.size(); ++i) {
                int sourceParamIndex = freeSourceParams.at(i);
                const int newParamWithSameTypeIndex = findUniqueTypeMatch(
                            sourceParamIndex, sourceFunction, newFunction,
                            freeSourceParams, freeNewParams);
                if (newParamWithSameTypeIndex != -1) {
                    sourceParamToNewParam[sourceParamIndex] = newParamWithSameTypeIndex;
                    newParamToSourceParam[newParamWithSameTypeIndex] = sourceParamIndex;
                }
            }

            // add position based binds if possible
            for (int i = 0; i < existingParamCount && i < newParamCount; ++i) {
                if (newParamToSourceParam[i] == -1 && sourceParamToNewParam[i] == -1) {
                    newParamToSourceParam[i] = i;
                    sourceParamToNewParam[i] = i;
                }
            }
        }

        // build the new parameter declarations
        QString newTargetParameters;
        bool hadChanges = newParamCount < existingParamCount; // below, additions and changes set this to true as well
        QHash<Symbol *, QString> renamedTargetParameters;
        bool switchedOnly = true;
        for (int newParamIndex = 0; newParamIndex < newParamCount; ++newParamIndex) {
            const int existingParamIndex = newParamToSourceParam[newParamIndex];
            Symbol *newParam = newFunction->argumentAt(newParamIndex);
            const bool isFirstNewParam = newParamIndex == 0;

            if (!isFirstNewParam)
                newTargetParameters += QLatin1Char(',');

            QString newTargetParam;

            // if it's genuinely new, add it
            if (existingParamIndex == -1) {
                FullySpecifiedType type = rewriteType(newParam->type(), &env, control);
                newTargetParam = overview.prettyType(type, newParam->name());
                hadChanges = true;
                switchedOnly = false;
            // otherwise preserve as much as possible from the existing parameter
            } else {
                Symbol *targetParam = targetFunction->argumentAt(existingParamIndex);
                Symbol *sourceParam = sourceFunction->argumentAt(existingParamIndex);
                ParameterDeclarationAST *targetParamAst = targetParameterDecls.at(existingParamIndex);

                int parameterStart = targetFile->endOf(targetFunctionDeclarator->lparen_token);
                if (existingParamIndex > 0) {
                    ParameterDeclarationAST *prevTargetParamAst = targetParameterDecls.at(existingParamIndex - 1);
                    const unsigned commaToken = findCommaTokenBetween(targetFile, prevTargetParamAst, targetParamAst);
                    if (commaToken > 0)
                        parameterStart = targetFile->endOf(commaToken);
                }

                int parameterEnd = targetFile->startOf(targetFunctionDeclarator->rparen_token);
                if (existingParamIndex + 1 < existingParamCount) {
                    ParameterDeclarationAST *nextTargetParamAst = targetParameterDecls.at(existingParamIndex + 1);
                    const unsigned commaToken = findCommaTokenBetween(targetFile, targetParamAst, nextTargetParamAst);
                    if (commaToken > 0)
                        parameterEnd = targetFile->startOf(commaToken);
                }

                // if the name wasn't changed, don't change the target name even if it's different
                const Name *replacementName = newParam->name();
                if (namesEqual(replacementName, sourceParam->name()))
                    replacementName = targetParam->name();

                // don't change the name if it's in a comment
                if (hasCommentedName(targetFile->cppDocument()->translationUnit(),
                                     QString::fromUtf8(targetFile->cppDocument()->utf8Source()),
                                     targetFunctionDeclarator, existingParamIndex))
                    replacementName = nullptr;

                // track renames
                if (replacementName != targetParam->name() && replacementName)
                    renamedTargetParameters[targetParam] = overview.prettyName(replacementName);

                // need to change the type (and name)?
                FullySpecifiedType replacementType = rewriteType(newParam->type(), &env, control);
                if (!newParam->type().match(sourceParam->type())
                        && !replacementType.match(targetParam->type())) {
                    switchedOnly = false;
                    const int parameterTypeStart = targetFile->startOf(targetParamAst);
                    int parameterTypeEnd = 0;
                    if (targetParamAst->declarator)
                        parameterTypeEnd = targetFile->endOf(targetParamAst->declarator);
                    else if (targetParamAst->type_specifier_list)
                        parameterTypeEnd = targetFile->endOf(targetParamAst->type_specifier_list->lastToken() - 1);
                    else
                        parameterTypeEnd = targetFile->startOf(targetParamAst);

                    newTargetParam = targetFile->textOf(parameterStart, parameterTypeStart);
                    newTargetParam += overview.prettyType(replacementType, replacementName);
                    newTargetParam += targetFile->textOf(parameterTypeEnd, parameterEnd);
                    hadChanges = true;
                // change the name only?
                } else if (!namesEqual(targetParam->name(), replacementName)) {
                    switchedOnly = false;
                    DeclaratorIdAST *id = getDeclaratorId(targetParamAst->declarator);
                    const QString &replacementNameStr = overview.prettyName(replacementName);
                    if (id) {
                        newTargetParam += targetFile->textOf(parameterStart, targetFile->startOf(id));
                        QString rest = targetFile->textOf(targetFile->endOf(id), parameterEnd);
                        if (replacementNameStr.isEmpty()) {
                            unsigned nextToken = targetFile->tokenAt(id->lastToken()).kind(); // token after id
                            if (nextToken == T_COMMA
                                    || nextToken == T_EQUAL
                                    || nextToken == T_RPAREN) {
                                if (nextToken != T_EQUAL)
                                    newTargetParam = newTargetParam.trimmed();
                                newTargetParam += rest.trimmed();
                            }
                        } else {
                            newTargetParam += replacementNameStr;
                            newTargetParam += rest;
                        }
                    } else {
                        // add name to unnamed parameter
                        int insertPos = parameterEnd;
                        if (targetParamAst->equal_token)
                            insertPos = targetFile->startOf(targetParamAst->equal_token);
                        newTargetParam += targetFile->textOf(parameterStart, insertPos);

                        // prepend a space, unless ' ', '*', '&'
                        QChar lastChar;
                        if (!newTargetParam.isEmpty())
                            lastChar = newTargetParam.at(newTargetParam.size() - 1);
                        if (!lastChar.isSpace() && lastChar != QLatin1Char('*') && lastChar != QLatin1Char('&'))
                            newTargetParam += QLatin1Char(' ');

                        newTargetParam += replacementNameStr;

                        // append a space, unless unnecessary
                        const QString &rest = targetFile->textOf(insertPos, parameterEnd);
                        if (!rest.isEmpty() && !rest.at(0).isSpace())
                            newTargetParam += QLatin1Char(' ');

                        newTargetParam += rest;
                    }
                    hadChanges = true;
                // change nothing - though the parameter might still have moved
                } else {
                    if (existingParamIndex != newParamIndex)
                        hadChanges = true;
                    newTargetParam = targetFile->textOf(parameterStart, parameterEnd);
                }
            }

            // apply
            newTargetParameters += ensureCorrectParameterSpacing(newTargetParam, isFirstNewParam);
        }
        if (hadChanges) {
            // Special case for when there was purely a parameter switch:
            // This operation can simply be mapped to the "flip" change operation, with
            // no heuristics as to the formatting.
            if (switchedOnly) {
                QList<int> srcIndices;
                for (int tgtIndex = 0; tgtIndex < newParamToSourceParam.size(); ++tgtIndex) {
                    if (srcIndices.contains(tgtIndex))
                        continue;
                    const int srcIndex = newParamToSourceParam[tgtIndex];
                    srcIndices << srcIndex;
                    const ParameterDeclarationAST * const srcDecl = targetParameterDecls.at(srcIndex);
                    const ParameterDeclarationAST * const tgtDecl = targetParameterDecls.at(tgtIndex);
                    const ChangeSet::Range srcRange = targetFile->range(srcDecl);
                    const ChangeSet::Range tgtRange = targetFile->range(tgtDecl);
                    changes.flip(srcRange, tgtRange);
                }
            } else {
                changes.replace(targetFile->endOf(targetFunctionDeclarator->lparen_token),
                                targetFile->startOf(targetFunctionDeclarator->rparen_token),
                                newTargetParameters);
            }
        }

        // Change parameter names in function documentation.
        [&] {
            if (renamedTargetParameters.isEmpty())
                return;
            const QList<Token> functionComments = commentsForDeclaration(
                    targetFunction, targetDeclaration, *targetFile->document(),
                targetFile->cppDocument());
            if (functionComments.isEmpty())
                return;
            const QString &content = targetFile->document()->toPlainText();
            const QStringView docView = QStringView(content);
            for (auto it = renamedTargetParameters.cbegin();
                 it != renamedTargetParameters.cend(); ++it) {
                if (!it.key()->name())
                    continue;
                const QString paramName = Overview().prettyName(it.key()->name());
                for (const Token &tok : functionComments) {
                    const TranslationUnit * const tu = targetFile->cppDocument()->translationUnit();
                    const int tokenStartPos = tu->getTokenPositionInDocument(
                        tok, targetFile->document());
                    const int tokenEndPos = tu->getTokenEndPositionInDocument(
                        tok, targetFile->document());
                    const QStringView tokenView = docView.mid(tokenStartPos,
                                                              tokenEndPos - tokenStartPos);
                    const QList<Text::Range> ranges = symbolOccurrencesInText(
                        *targetFile->document(), tokenView, tokenStartPos, paramName);
                    for (const Text::Range &r : ranges) {
                        const int startPos = r.begin.toPositionInDocument(targetFile->document());
                        const int endPos = r.end.toPositionInDocument(targetFile->document());
                        changes.replace(startPos, endPos, it.value());
                    }
                }
            }
        }();

        // for function definitions, rename the local usages
        FunctionDefinitionAST *targetDefinition = targetDeclaration->asFunctionDefinition();
        if (targetDefinition && !renamedTargetParameters.isEmpty()) {
            const LocalSymbols localSymbols(targetFile->cppDocument(), {}, targetDefinition);
            const int endOfArguments = targetFile->endOf(targetFunctionDeclarator->rparen_token);

            for (auto it = renamedTargetParameters.cbegin(), end = renamedTargetParameters.cend();
                    it != end; ++it) {
                const QList<SemanticInfo::Use> &uses = localSymbols.uses.value(it.key());
                for (const SemanticInfo::Use &use : uses) {
                    if (use.isInvalid())
                        continue;
                    const int useStart = targetFile->position(use.line, use.column);
                    if (useStart <= endOfArguments)
                        continue;
                    changes.replace(useStart, useStart + use.length, it.value());
                }
            }
        }
    }

    // sync cv qualification
    if (targetFunction->isConst() != newFunction->isConst()
            || targetFunction->isVolatile() != newFunction->isVolatile()) {
        QString cvString;
        if (newFunction->isConst())
            cvString += QLatin1String("const");
        if (newFunction->isVolatile()) {
            if (!cvString.isEmpty())
                cvString += QLatin1Char(' ');
            cvString += QLatin1String("volatile");
        }

        // if the target function is neither const or volatile, just add the new specifiers after the closing ')'
        if (!targetFunction->isConst() && !targetFunction->isVolatile()) {
            cvString.prepend(QLatin1Char(' '));
            changes.insert(targetFile->endOf(targetFunctionDeclarator->rparen_token), cvString);
        // modify/remove existing specifiers
        } else {
            SimpleSpecifierAST *constSpecifier = nullptr;
            SimpleSpecifierAST *volatileSpecifier = nullptr;
            for (SpecifierListAST *it = targetFunctionDeclarator->cv_qualifier_list; it; it = it->next) {
                if (SimpleSpecifierAST *simple = it->value->asSimpleSpecifier()) {
                    unsigned kind = targetFile->tokenAt(simple->specifier_token).kind();
                    if (kind == T_CONST)
                        constSpecifier = simple;
                    else if (kind == T_VOLATILE)
                        volatileSpecifier = simple;
                }
            }
            // if there are both, we just need to remove
            if (constSpecifier && volatileSpecifier) {
                if (!newFunction->isConst())
                    changes.remove(targetFile->endOf(constSpecifier->specifier_token - 1), targetFile->endOf(constSpecifier));
                if (!newFunction->isVolatile())
                    changes.remove(targetFile->endOf(volatileSpecifier->specifier_token - 1), targetFile->endOf(volatileSpecifier));
            // otherwise adjust, remove or extend the one existing specifier
            } else {
                SimpleSpecifierAST *specifier = constSpecifier ? constSpecifier : volatileSpecifier;
                QTC_ASSERT(specifier, return changes);

                if (!newFunction->isConst() && !newFunction->isVolatile())
                    changes.remove(targetFile->endOf(specifier->specifier_token - 1), targetFile->endOf(specifier));
                else
                    changes.replace(targetFile->range(specifier), cvString);
            }
        }
    }

    // sync noexcept/throw()
    const QString exceptionSpecTarget = targetFunction->exceptionSpecification()
            ? QString::fromUtf8(targetFunction->exceptionSpecification()->chars()) : QString();
    const QString exceptionSpecNew = newFunction->exceptionSpecification()
            ? QString::fromUtf8(newFunction->exceptionSpecification()->chars()) : QString();
    if (exceptionSpecTarget != exceptionSpecNew) {
        if (!exceptionSpecTarget.isEmpty() && !exceptionSpecNew.isEmpty()) {
            changes.replace(targetFile->range(targetFunctionDeclarator->exception_specification),
                            exceptionSpecNew);
        } else if (exceptionSpecTarget.isEmpty()) {
            int previousToken = targetFunctionDeclarator->ref_qualifier_token;
            if (!previousToken) {
                const SpecifierListAST *cvList = targetFunctionDeclarator->cv_qualifier_list;
                if (cvList && cvList->lastValue()->asSimpleSpecifier())
                    previousToken = cvList->lastValue()->asSimpleSpecifier()->specifier_token;
            }
            if (!previousToken)
                previousToken = targetFunctionDeclarator->rparen_token;
            changes.insert(targetFile->endOf(previousToken), ' ' + exceptionSpecNew);
        } else if (!exceptionSpecTarget.isEmpty()) {
            changes.remove(targetFile->range(targetFunctionDeclarator->exception_specification));
        }
    }

    if (targetOffset != -1) {
        // move all change operations to have the right start offset
        const int moveAmount = targetOffset - targetFile->startOf(targetDeclaration);
        QList<ChangeSet::EditOp> ops = changes.operationList();
        for (int i = 0; i < ops.size(); ++i) {
            ops[i].pos1 += moveAmount;
            ops[i].pos2 += moveAmount;
        }
        changes = ChangeSet(ops);
    }

    return changes;
}

// Only has an effect with operators.
// Makes sure there is exactly one space between the "operator" string
// and the actual operator, as that is what it will be compared against.
QString FunctionDeclDefLink::normalizedInitialName() const
{
    QString n = nameInitial;
    const QString op = "operator";
    int index = n.indexOf(op);
    if (index == -1)
        return n;
    if (index > 0 && n.at(index - 1).isLetterOrNumber())
        return n;
    index += op.length();
    if (index == n.length())
        return n;
    if (n.at(index).isLetterOrNumber())
        return n;
    n.insert(index++, ' ');
    while (index < n.length() && n.at(index) == ' ')
        n.remove(index, 1);
    return n;
}

} // namespace Internal
} // namespace CppEditor
