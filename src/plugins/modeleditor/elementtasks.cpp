// Copyright (C) 2016 Jochen Becher
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "elementtasks.h"

#include "componentviewcontroller.h"
#include "modeleditor_plugin.h"
#include "modeleditortr.h"
#include "modelsmanager.h"
#include "openelementvisitor.h"

#include "qmt/diagram/delement.h"
#include "qmt/diagram/dpackage.h"
#include "qmt/diagram_controller/dselection.h"
#include "qmt/diagram_scene/diagramscenemodel.h"
#include "qmt/diagram_ui/diagramsmanager.h"
#include "qmt/document_controller/documentcontroller.h"
#include "qmt/infrastructure/contextmenuaction.h"
#include "qmt/model/melement.h"
#include "qmt/model/mclass.h"
#include "qmt/model/mdiagram.h"
#include "qmt/model/mcanvasdiagram.h"
#include "qmt/model/mpackage.h"
#include "qmt/model_controller/modelcontroller.h"
#include "qmt/model_widgets_ui/addrelatedelementsdialog.h"
#include "qmt/tasks/finddiagramvisitor.h"
#include "qmt/project_controller/projectcontroller.h"
#include "qmt/project/project.h"

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/editormanager/ieditorfactory.h>
#include <coreplugin/icore.h>
#include <cppeditor/cpplocatordata.h>
#include <cppeditor/indexitem.h>
#include <cppeditor/searchsymbols.h>
#include <extensionsystem/pluginmanager.h>
#include <utils/qtcassert.h>

#include <QMenu>
#include <QMessageBox>

using namespace Core;
using namespace CppEditor;
using Utils::FilePath;

namespace ModelEditor {
namespace Internal {

class ElementTasks::ElementTasksPrivate {
public:
    qmt::DocumentController *documentController = nullptr;
    ComponentViewController *componentViewController = nullptr;
    QScopedPointer<qmt::AddRelatedElementsDialog> addRelatedElementsDialog;
};

ElementTasks::ElementTasks(QObject *parent)
    : QObject(parent),
      d(new ElementTasksPrivate)
{
    d->addRelatedElementsDialog.reset(new qmt::AddRelatedElementsDialog(Core::ICore::dialogParent()));
}

ElementTasks::~ElementTasks()
{
    delete d;
}

void ElementTasks::setDocumentController(qmt::DocumentController *documentController)
{
    d->documentController = documentController;
    d->addRelatedElementsDialog->setDiagramSceneController(documentController->diagramSceneController());
}

void ElementTasks::setComponentViewController(ComponentViewController *componentViewController)
{
    d->componentViewController = componentViewController;
}

void ElementTasks::openElement(const qmt::MElement *element)
{
    OpenModelElementVisitor visitor;
    visitor.setElementTasks(this);
    element->accept(&visitor);
}

void ElementTasks::openElement(const qmt::DElement *element, const qmt::MDiagram *diagram)
{
    Q_UNUSED(diagram)

    OpenDiagramElementVisitor visitor;
    visitor.setModelController(d->documentController->modelController());
    visitor.setElementTasks(this);
    element->accept(&visitor);
}

bool ElementTasks::hasClassDefinition(const qmt::MElement *element) const
{
    if (auto klass = dynamic_cast<const qmt::MClass *>(element)) {
        const QString qualifiedClassName = klass->umlNamespace().isEmpty() ? klass->name()
                                         : klass->umlNamespace() + "::" + klass->name();
        auto *locatorData = CppModelManager::locatorData();
        if (!locatorData)
            return false;
        const QList<IndexItem::Ptr> matches = locatorData->findSymbols(IndexItem::Class,
                                                                       qualifiedClassName);
        for (const IndexItem::Ptr &info : matches) {
            if (info->scopedSymbolName() == qualifiedClassName)
                return true;
        }
    }
    return false;
}

bool ElementTasks::hasClassDefinition(const qmt::DElement *element,
                                      const qmt::MDiagram *diagram) const
{
    Q_UNUSED(diagram)

    qmt::MElement *melement = d->documentController->modelController()->findElement(
                element->modelUid());
    if (!melement)
        return false;
    return hasClassDefinition(melement);
}

void ElementTasks::openClassDefinition(const qmt::MElement *element)
{
    if (auto klass = dynamic_cast<const qmt::MClass *>(element)) {
        const QString qualifiedClassName = klass->umlNamespace().isEmpty() ? klass->name()
                                         : klass->umlNamespace() + "::" + klass->name();

        auto *locatorData = CppModelManager::locatorData();
        if (!locatorData)
            return;
        const QList<IndexItem::Ptr> matches = locatorData->findSymbols(IndexItem::Class,
                                                                       qualifiedClassName);
        for (const IndexItem::Ptr &info : matches) {
            if (info->scopedSymbolName() != qualifiedClassName)
                continue;
            if (EditorManager::openEditorAt({info->filePath(), info->line(), info->column()}))
                return;
        }
    }
}

void ElementTasks::openClassDefinition(const qmt::DElement *element, const qmt::MDiagram *diagram)
{
    Q_UNUSED(diagram)

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return;
    openClassDefinition(melement);
}

bool ElementTasks::hasHeaderFile(const qmt::MElement *element) const
{
    // TODO implement
    Q_UNUSED(element)
    return false;
}

bool ElementTasks::hasHeaderFile(const qmt::DElement *element, const qmt::MDiagram *diagram) const
{
    Q_UNUSED(diagram)

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return false;
    return hasHeaderFile(melement);
}

bool ElementTasks::hasSourceFile(const qmt::MElement *element) const
{
    // TODO implement
    Q_UNUSED(element)
    return false;
}

bool ElementTasks::hasSourceFile(const qmt::DElement *element, const qmt::MDiagram *diagram) const
{
    Q_UNUSED(diagram)

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return false;
    return hasSourceFile(melement);
}

void ElementTasks::openHeaderFile(const qmt::MElement *element)
{
    // TODO implement
    Q_UNUSED(element)
}

void ElementTasks::openHeaderFile(const qmt::DElement *element, const qmt::MDiagram *diagram)
{
    Q_UNUSED(diagram)

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return;
    openHeaderFile(melement);
}

void ElementTasks::openSourceFile(const qmt::MElement *element)
{
    // TODO implement
    Q_UNUSED(element)
}

void ElementTasks::openSourceFile(const qmt::DElement *element, const qmt::MDiagram *diagram)
{
    Q_UNUSED(diagram)

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return;
    openSourceFile(melement);
}

bool ElementTasks::hasFolder(const qmt::MElement *element) const
{
    // TODO implement
    Q_UNUSED(element)
    return false;
}

bool ElementTasks::hasFolder(const qmt::DElement *element, const qmt::MDiagram *diagram) const
{
    Q_UNUSED(diagram)

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return false;
    return hasFolder(melement);
}

void ElementTasks::showFolder(const qmt::MElement *element)
{
    // TODO implement
    Q_UNUSED(element)
}

void ElementTasks::showFolder(const qmt::DElement *element, const qmt::MDiagram *diagram)
{
    Q_UNUSED(diagram)

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return;
    showFolder(melement);
}

bool ElementTasks::hasDiagram(const qmt::MElement *element) const
{
    qmt::FindDiagramVisitor visitor;
    element->accept(&visitor);
    const qmt::MDiagram *diagram = visitor.diagram();
    return diagram != nullptr;
}

bool ElementTasks::hasDiagram(const qmt::DElement *element, const qmt::MDiagram *diagram) const
{
    Q_UNUSED(diagram)

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return false;
    return hasDiagram(melement);
}

void ElementTasks::openDiagram(const qmt::MElement *element)
{
    qmt::FindDiagramVisitor visitor;
    element->accept(&visitor);
    const qmt::MDiagram *diagram = visitor.diagram();
    if (diagram) {
        ModelEditorPlugin::modelsManager()->openDiagram(
                    d->documentController->projectController()->project()->uid(),
                    diagram->uid());
    }
}

void ElementTasks::openDiagram(const qmt::DElement *element, const qmt::MDiagram *diagram)
{
    Q_UNUSED(diagram)

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return;
    openDiagram(melement);
}

bool ElementTasks::hasParentDiagram(const qmt::MElement *element) const
{
    while (element && element->owner()) {
        qmt::MObject *parentObject = element->owner()->owner();
        if (parentObject) {
            qmt::FindDiagramVisitor visitor;
            parentObject->accept(&visitor);
            const qmt::MDiagram *parentDiagram = visitor.diagram();
            if (parentDiagram) {
                return true;
            }
        }
        element = element->owner();
    }
    return false;
}

bool ElementTasks::hasParentDiagram(const qmt::DElement *element, const qmt::MDiagram *diagram) const
{
    Q_UNUSED(diagram)

    if (!element)
        return false;

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return false;
    return hasParentDiagram(melement);
}

void ElementTasks::openParentDiagram(const qmt::MElement *element)
{
    while (element && element->owner()) {
        qmt::MObject *parentObject = element->owner()->owner();
        if (parentObject) {
            qmt::FindDiagramVisitor visitor;
            parentObject->accept(&visitor);
            const qmt::MDiagram *parentDiagram = visitor.diagram();
            if (parentDiagram) {
                ModelEditorPlugin::modelsManager()->openDiagram(
                            d->documentController->projectController()->project()->uid(),
                            parentDiagram->uid());
                return;
            }
        }
        element = element->owner();
    }
}

void ElementTasks::openParentDiagram(const qmt::DElement *element, const qmt::MElement *diagram)
{
    Q_UNUSED(diagram)

    if (!element)
        return;

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return;
    openParentDiagram(melement);
}

bool ElementTasks::mayCreateDiagram(const qmt::MElement *element) const
{
    return dynamic_cast<const qmt::MPackage *>(element) != nullptr;
}

bool ElementTasks::mayCreateDiagram(const qmt::DElement *element,
                                    const qmt::MDiagram *diagram) const
{
    Q_UNUSED(diagram)

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return false;
    return mayCreateDiagram(melement);
}

void ElementTasks::createAndOpenDiagram(const qmt::MElement *element)
{
    if (auto package = dynamic_cast<const qmt::MPackage *>(element)) {
        qmt::FindDiagramVisitor visitor;
        element->accept(&visitor);
        const qmt::MDiagram *diagram = visitor.diagram();
        if (diagram) {
            ModelEditorPlugin::modelsManager()->openDiagram(
                        d->documentController->projectController()->project()->uid(),
                        diagram->uid());
        } else {
            auto newDiagram = new qmt::MCanvasDiagram();
            newDiagram->setName(package->name());
            qmt::MPackage *parentPackage = d->documentController->modelController()->findObject<qmt::MPackage>(package->uid());
            QMT_ASSERT(parentPackage, delete newDiagram; return);
            d->documentController->modelController()->addObject(parentPackage, newDiagram);
            ModelEditorPlugin::modelsManager()->openDiagram(
                        d->documentController->projectController()->project()->uid(),
                        newDiagram->uid());
        }
    }
}

void ElementTasks::createAndOpenDiagram(const qmt::DElement *element, const qmt::MDiagram *diagram)
{
    Q_UNUSED(diagram)

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return;
    createAndOpenDiagram(melement);
}

FilePath ElementTasks::linkedFile(const qmt::MObject *mobject) const
{
    const QString filepath = mobject->linkedFileName();
    if (filepath.isEmpty())
        return {};

    FilePath projectName = d->documentController->projectController()->project()->fileName();
    return projectName.absolutePath().resolvePath(filepath).canonicalPath();
}

bool ElementTasks::hasLinkedFile(const qmt::MElement *element) const
{
    if (auto mobject = dynamic_cast<const qmt::MObject *>(element)) {
        FilePath filepath = linkedFile(mobject);
        if (!filepath.isEmpty())
            return filepath.exists();
    }
    return false;
}

bool ElementTasks::hasLinkedFile(const qmt::DElement *element, const qmt::MDiagram *diagram) const
{
    Q_UNUSED(diagram)

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return false;
    return hasLinkedFile(melement);
}

void ElementTasks::openLinkedFile(const qmt::MElement *element)
{
    if (auto mobject = dynamic_cast<const qmt::MObject *>(element)) {
        FilePath filepath = linkedFile(mobject);
        if (!filepath.isEmpty()) {
            if (filepath.exists()) {
                Core::EditorFactories list = Core::IEditorFactory::preferredEditorFactories(filepath);
                if (list.empty() || (list.count() <= 1 && list.at(0)->id() == "Core.BinaryEditor")) {
                    // intentionally ignore return code
                    (void) Core::EditorManager::openExternalEditor(filepath, "CorePlugin.OpenWithSystemEditor");
                } else {
                    // intentionally ignore return code
                    (void) Core::EditorManager::openEditor(filepath);
                }
            } else {
                QMessageBox::critical(
                    Core::ICore::dialogParent(),
                    Tr::tr("Opening File"),
                    Tr::tr("File \"%1\" does not exist.").arg(filepath.toUserOutput()));
            }
        }
    }
}

void ElementTasks::openLinkedFile(const qmt::DElement *element, const qmt::MDiagram *diagram)
{
    Q_UNUSED(diagram)

    qmt::MElement *melement = d->documentController->modelController()->findElement(element->modelUid());
    if (!melement)
        return;
    openLinkedFile(melement);
}

bool ElementTasks::extendContextMenu(const qmt::DElement *delement, const qmt::MDiagram *, QMenu *menu)
{
    bool extended = false;
    if (dynamic_cast<const qmt::DObject *>(delement)) {
        menu->addAction(new qmt::ContextMenuAction(Tr::tr("Add Related Elements..."), "addRelatedElementsDialog", menu));
        extended = true;
    }
    if (dynamic_cast<const qmt::DPackage *>(delement)) {
        menu->addAction(new qmt::ContextMenuAction(Tr::tr("Update Include Dependencies"), "updateIncludeDependencies", menu));
        extended = true;
    }
    return extended;
}

bool ElementTasks::handleContextMenuAction(qmt::DElement *element, qmt::MDiagram *diagram, const QString &id)
{
    if (id == "addRelatedElementsDialog") {
        qmt::DSelection selection = d->documentController->diagramsManager()->diagramSceneModel(diagram)->selectedElements();
        d->addRelatedElementsDialog->setElements(selection, diagram);
        d->addRelatedElementsDialog->open();
        return true;
    } else if (id == "updateIncludeDependencies") {
        qmt::MPackage *mpackage = d->documentController->modelController()->findElement<qmt::MPackage>(element->modelUid());
        if (mpackage)
            d->componentViewController->updateIncludeDependencies(mpackage);
        return true;
    }
    return false;
}

} // namespace Internal
} // namespace ModelEditor
