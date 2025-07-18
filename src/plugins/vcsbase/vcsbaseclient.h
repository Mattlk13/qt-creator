// Copyright (C) 2016 Brian McGillion and Hugues Delorme
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "vcsbase_global.h"
#include "vcsbaseclientsettings.h"
#include "vcscommand.h"
#include "vcsenums.h"

#include <solutions/tasking/tasktreerunner.h>

#include <utils/id.h>
#include <utils/processenums.h>
#include <utils/textcodec.h>

#include <QStringList>
#include <QVariant>

#include <functional>

QT_BEGIN_NAMESPACE
class QToolBar;
QT_END_NAMESPACE

namespace Utils { class Process; }

namespace VcsBase {

class VcsBaseEditorConfig;
class VcsBaseEditorWidget;

using CommandHandler = std::function<void(const CommandResult &)>;

class VCSBASE_EXPORT VcsCommandData
{
public:
    Utils::FilePath workingDirectory;
    QStringList arguments;
    RunFlags flags = RunFlags::None;
    Core::ProgressParser progressParser = {};
    Utils::TextEncoding encoding = {};
    CommandHandler commandHandler = {};
};

class VCSBASE_EXPORT VcsBaseClientImpl : public QObject
{
public:
    explicit VcsBaseClientImpl(VcsBaseSettings *baseSettings);
    ~VcsBaseClientImpl() override = default;

    virtual Utils::FilePath vcsBinary(const Utils::FilePath &forDirectory) const;
    int vcsTimeoutS() const;

    VcsBaseEditorWidget *createVcsEditor(Utils::Id kind, QString title,
                                         const Utils::FilePath &source,
                                         const Utils::TextEncoding &encoding,
                                         const char *registerDynamicProperty,
                                         const QString &dynamicPropertyValue) const;

    void setupCommand(Utils::Process &process,
                      const Utils::FilePath &workingDirectory,
                      const QStringList &args) const;

    virtual Utils::Environment processEnvironment(const Utils::FilePath &appliedTo) const;

    // VCS functionality:
    virtual void annotate(const Utils::FilePath &workingDir, const QString &file,
                          int lineNumber = -1, const QString &revision = {},
                          const QStringList &extraOptions = {}, int firstLine = -1) = 0;

    static QStringList splitLines(const QString &s);

    static QString stripLastNewline(const QString &in);

    // Fully synchronous VCS execution (QProcess-based)
    CommandResult vcsSynchronousExec(const Utils::FilePath &workingDir,
                                     const QStringList &args, RunFlags flags = RunFlags::None,
                                     int timeoutS = -1, const Utils::TextEncoding &encoding = {}) const;
    CommandResult vcsSynchronousExec(const Utils::FilePath &workingDir,
                                     const Utils::CommandLine &cmdLine,
                                     RunFlags flags = RunFlags::None,
                                     int timeoutS = -1, const Utils::TextEncoding &encoding = {}) const;

    void executeInEditor(const Utils::FilePath &workingDirectory,
                         const QStringList &arguments,
                         VcsBaseEditorWidget *editor) const;
    void executeInEditor(const Utils::FilePath &workingDirectory,
                         const Utils::CommandLine &command,
                         VcsBaseEditorWidget *editor) const;
    void enqueueTask(const Tasking::ExecutableItem &task);
    Tasking::ExecutableItem commandTask(const VcsCommandData &data) const;
    void enqueueCommand(const VcsCommandData &data);

protected:
    void resetCachedVcsInfo(const Utils::FilePath &workingDir);
    virtual void annotateRevisionRequested(const Utils::FilePath &workingDirectory, const QString &file,
                                           const QString &change, int line);

private:
    void saveSettings();
    void startNextTask();

    VcsBaseSettings *m_baseSettings = nullptr; // Aspect based.
    Tasking::TaskTreeRunner m_taskTreeRunner;
    QList<Tasking::ExecutableItem> m_taskQueue;
};

class VCSBASE_EXPORT VcsBaseClient : public VcsBaseClientImpl
{
    Q_OBJECT

public:
    class VCSBASE_EXPORT StatusItem
    {
    public:
        StatusItem() = default;
        QString flags;
        QString file;
    };

    explicit VcsBaseClient(VcsBaseSettings *baseSettings);

    virtual bool synchronousCreateRepository(const Utils::FilePath &workingDir,
                                             const QStringList &extraOptions = {});
    virtual bool synchronousAdd(const Utils::FilePath &workingDir,
                                const QString &relFileName,
                                const QStringList &extraOptions = {});
    virtual bool synchronousRemove(const Utils::FilePath &workingDir,
                                   const QString &fileName,
                                   const QStringList &extraOptions = {});
    virtual bool synchronousMove(const Utils::FilePath &workingDir,
                                 const Utils::FilePath &from, const Utils::FilePath &to,
                                 const QStringList &extraOptions = {});
    void pull(const Utils::FilePath &workingDir, const QString &srcLocation,
              const QStringList &extraOptions = {}, const CommandHandler &commandHandler = {});
    void push(const Utils::FilePath &workingDir, const QString &dstLocation,
              const QStringList &extraOptions = {});
    void annotate(const Utils::FilePath &workingDir, const QString &file,
                  int lineNumber = -1, const QString &revision = {},
                  const QStringList &extraOptions = {}, int firstLine = -1) override;
    void diff(const Utils::FilePath &workingDir, const QStringList &files = {});
    virtual void log(const Utils::FilePath &workingDir,
                     const QStringList &files = {},
                     const QStringList &extraOptions = {},
                     bool enableAnnotationContextMenu = false,
                     const std::function<void(Utils::CommandLine &)> &addAuthOptions = {});
    virtual void status(const Utils::FilePath &workingDir,
                        const QString &file = {},
                        const QStringList &extraOptions = {});
    virtual void emitParsedStatus(const Utils::FilePath &repository,
                                  const QStringList &extraOptions = {});
    virtual void revertFile(const Utils::FilePath &workingDir,
                            const QString &file,
                            const QString &revision = {},
                            const QStringList &extraOptions = {});
    virtual void revertAll(const Utils::FilePath &workingDir,
                           const QString &revision = {},
                           const QStringList &extraOptions = {});
    virtual void import(const Utils::FilePath &repositoryRoot,
                        const QStringList &files,
                        const QStringList &extraOptions = {});
    virtual void update(const Utils::FilePath &repositoryRoot,
                        const QString &revision = {},
                        const QStringList &extraOptions = {});
    virtual void commit(const Utils::FilePath &repositoryRoot,
                        const QStringList &files,
                        const QString &commitMessageFile,
                        const QStringList &extraOptions = {});

    virtual void view(const Utils::FilePath &source, const QString &id,
                      const QStringList &extraOptions = QStringList());

signals:
    void parsedStatus(const QList<VcsBase::VcsBaseClient::StatusItem> &statusList);
    // Passes on changed signals from VcsJob to Control
    void repositoryChanged(const Utils::FilePath &repository);
    void filesChanged(const Utils::FilePaths &files);

public:
    enum VcsCommandTag
    {
        CreateRepositoryCommand,
        CloneCommand,
        AddCommand,
        RemoveCommand,
        MoveCommand,
        PullCommand,
        PushCommand,
        CommitCommand,
        ImportCommand,
        UpdateCommand,
        RevertCommand,
        AnnotateCommand,
        DiffCommand,
        LogCommand,
        StatusCommand
    };
protected:
    virtual QString vcsCommandString(VcsCommandTag cmd) const;
    virtual Utils::Id vcsEditorKind(VcsCommandTag cmd) const = 0;
    virtual ExitCodeInterpreter exitCodeInterpreter(VcsCommandTag) const { return {}; }

    virtual QStringList revisionSpec(const QString &/*revision*/) const { return {}; }

    typedef std::function<VcsBaseEditorConfig *(QToolBar *)> ConfigCreator;
    void setDiffConfigCreator(ConfigCreator creator);
    void setLogConfigCreator(ConfigCreator creator);

    virtual StatusItem parseStatusLine(const QString &/*line*/) const { return {}; }

    QString vcsEditorTitle(const QString &vcsCmd, const QString &sourceId) const;

private:
    void statusParser(const QString &);

    VcsBaseClient::ConfigCreator m_diffConfigCreator;
    VcsBaseClient::ConfigCreator m_logConfigCreator;
};

} //namespace VcsBase
