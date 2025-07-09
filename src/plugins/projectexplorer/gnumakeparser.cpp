// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "gnumakeparser.h"

#include "task.h"

#include <utils/qtcassert.h>
#include <utils/temporaryfile.h>

#include <QDir>
#include <QFile>

#ifdef WITH_TESTS
#include "outputparser_test.h"
#include <QTest>
#endif

using namespace Utils;

namespace ProjectExplorer {

namespace {
class MakeTask : public BuildSystemTask
{
public:
    MakeTask(TaskType type, const QString &description, const Utils::FilePath &file = {},
             int line = -1) : BuildSystemTask(type, description, file, line)
    {
        setOrigin("make");
    }
};

// optional full path, make executable name, optional exe extension, optional number in square brackets, colon space
const char * const MAKEEXEC_PATTERN("^(.*?[/\\\\])?(mingw(32|64)-|g)?make(.exe)?(\\[\\d+\\])?:\\s");
const char * const MAKEFILE_PATTERN("^((.*?[/\\\\])?[Mm]akefile(\\.[a-zA-Z]+)?):(\\d+):\\s");
}

GnuMakeParser::GnuMakeParser()
{
    setObjectName(QLatin1String("GnuMakeParser"));
    m_makeDir.setPattern(QLatin1String(MAKEEXEC_PATTERN) +
                         QLatin1String("(\\w+) directory .(.+).$"));
    QTC_CHECK(m_makeDir.isValid());
    m_makeLine.setPattern(QLatin1String(MAKEEXEC_PATTERN) + QLatin1String("(.*)$"));
    QTC_CHECK(m_makeLine.isValid());
    m_errorInMakefile.setPattern(QLatin1String(MAKEFILE_PATTERN) + QLatin1String("(.*)$"));
    QTC_CHECK(m_errorInMakefile.isValid());
}

class Result {
public:
    Result() = default;

    QString description;
    bool isFatal = false;
    Task::TaskType type = Task::Error;
};

static Task::TaskType taskTypeFromDescription(const QString &description)
{
    if (description.contains(". Stop."))
        return Task::Error;
    if (description.contains("not found"))
        return Task::Error;
    if (description.contains("No rule to make target"))
        return Task::Error;
    // Extend as needed.
    return Task::Warning;
}

static Result parseDescription(const QString &description)
{
    Result result;
    if (description.startsWith(QLatin1String("warning: "), Qt::CaseInsensitive)) {
        result.description = description.mid(9);
        result.type = Task::Warning;
        result.isFatal = false;
    } else if (description.startsWith(QLatin1String("*** "))) {
        result.description = description.mid(4);
        result.type = Task::Error;
        result.isFatal = true;
    } else {
        result.description = description;
        result.type = taskTypeFromDescription(description);
        result.isFatal = false;
    }
    return result;
}

void GnuMakeParser::emitTask(const ProjectExplorer::Task &task)
{
    if (task.isError()) // Assume that all make errors will be follow up errors.
        m_suppressIssues = true;
    scheduleTask(task, 1, 0);
}

OutputLineParser::Result GnuMakeParser::handleLine(const QString &line, OutputFormat type)
{
    const QString lne = rightTrimmed(line);
    if (type == StdOutFormat) {
        QRegularExpressionMatch match = m_makeDir.match(lne);
        if (match.hasMatch()) {
            if (match.captured(6) == QLatin1String("Leaving"))
                emit searchDirExpired(FilePath::fromString(match.captured(7)));
            else
                emit newSearchDirFound(FilePath::fromString(match.captured(7)));
            return Status::Done;
        }
        return Status::NotHandled;
    }
    QRegularExpressionMatch match = m_errorInMakefile.match(lne);
    if (match.hasMatch()) {
        ProjectExplorer::Result res = parseDescription(match.captured(5));
        if (res.isFatal)
            ++m_fatalErrorCount;
        LinkSpecs linkSpecs;
        if (!m_suppressIssues) {
            const FilePath file = absoluteFilePath(FilePath::fromUserInput(match.captured(1)));
            const int lineNo = match.captured(4).toInt();
            addLinkSpecForAbsoluteFilePath(linkSpecs, file, lineNo, -1, match, 1);
            emitTask(MakeTask(res.type, res.description, file, lineNo));
        }
        return {Status::Done, linkSpecs};
    }
    match = m_makeLine.match(lne);
    if (match.hasMatch()) {
        ProjectExplorer::Result res = parseDescription(match.captured(6));
        if (res.isFatal)
            ++m_fatalErrorCount;
        if (!m_suppressIssues)
            emitTask(MakeTask(res.type, res.description));
        return Status::Done;
    }

    return Status::NotHandled;
}

bool GnuMakeParser::hasFatalErrors() const
{
    return m_fatalErrorCount > 0;
}

#ifdef WITH_TESTS
namespace Internal {
class GnuMakeParserTester : public QObject
{
public:
    explicit GnuMakeParserTester(GnuMakeParser *p, QObject *parent = nullptr) :
        QObject(parent),
        parser(p)
    { }

    void parserIsAboutToBeDeleted()
    {
        directories = parser->searchDirectories();
    }

    FilePaths directories;
    GnuMakeParser *parser;
};

class GnuMakeParserTest : public QObject
{
    Q_OBJECT

private slots:
    void testParsing_data()
    {
        QTest::addColumn<QStringList>("extraSearchDirs");
        QTest::addColumn<QString>("input");
        QTest::addColumn<OutputParserTester::Channel>("inputChannel");
        QTest::addColumn<QStringList>("childStdOutLines");
        QTest::addColumn<QStringList>("childStdErrLines");
        QTest::addColumn<Tasks>("tasks");
        QTest::addColumn<QStringList>("additionalSearchDirs");

        QTest::newRow("pass-through stdout")
            << QStringList()
            << QString::fromLatin1("Sometext") << OutputParserTester::STDOUT
            << QStringList("Sometext") << QStringList()
            << Tasks()
            << QStringList();
        QTest::newRow("pass-through stderr")
            << QStringList()
            << QString::fromLatin1("Sometext") << OutputParserTester::STDERR
            << QStringList() << QStringList("Sometext")
            << Tasks()
            << QStringList();
        QTest::newRow("pass-through gcc infos")
            << QStringList()
            << QString::fromLatin1("/temp/test/untitled8/main.cpp: In function `int main(int, char**)':\n"
                                   "../../scriptbug/main.cpp: At global scope:\n"
                                   "../../scriptbug/main.cpp: In instantiation of void bar(i) [with i = double]:\n"
                                   "../../scriptbug/main.cpp:8: instantiated from void foo(i) [with i = double]\n"
                                   "../../scriptbug/main.cpp:22: instantiated from here")
            << OutputParserTester::STDERR
            << QStringList()
            << QStringList{"/temp/test/untitled8/main.cpp: In function `int main(int, char**)':",
                           "../../scriptbug/main.cpp: At global scope:",
                           "../../scriptbug/main.cpp: In instantiation of void bar(i) [with i = double]:",
                           "../../scriptbug/main.cpp:8: instantiated from void foo(i) [with i = double]",
                           "../../scriptbug/main.cpp:22: instantiated from here"}
            << Tasks()
            << QStringList();

        // make sure adding directories works (once;-)
        QTest::newRow("entering directory")
            << QStringList("/test/dir")
            << QString::fromLatin1("make[4]: Entering directory `/home/code/build/qt/examples/opengl/grabber'\n"
                                   "make[4]: Entering directory `/home/code/build/qt/examples/opengl/grabber'")
            << OutputParserTester::STDOUT
            << QStringList() << QStringList()
            << Tasks()
            << QStringList({"/home/code/build/qt/examples/opengl/grabber",
                            "/home/code/build/qt/examples/opengl/grabber", "/test/dir"});
        QTest::newRow("leaving directory")
            << QStringList({"/home/code/build/qt/examples/opengl/grabber", "/test/dir"})
            << QString::fromLatin1("make[4]: Leaving directory `/home/code/build/qt/examples/opengl/grabber'")
            << OutputParserTester::STDOUT
            << QStringList() << QStringList()
            << Tasks()
            << QStringList("/test/dir");

        QTest::newRow("make error")
            << QStringList()
            << QString::fromLatin1("make: *** No rule to make target `hello.c', needed by `hello.o'.  Stop.")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << MakeTask(Task::Error,
                            "No rule to make target `hello.c', needed by `hello.o'.  Stop."))
            << QStringList();

        QTest::newRow("multiple fatals")
            << QStringList()
            << QString::fromLatin1("make[3]: *** [.obj/debug-shared/gnumakeparser.o] Error 1\n"
                                   "make[3]: *** Waiting for unfinished jobs....\n"
                                   "make[2]: *** [sub-projectexplorer-make_default] Error 2")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << MakeTask(Task::Error,
                            "[.obj/debug-shared/gnumakeparser.o] Error 1"))
            << QStringList();

        QTest::newRow("Makefile error")
            << QStringList()
            << QString::fromLatin1("Makefile:360: *** missing separator (did you mean TAB instead of 8 spaces?). Stop.")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << MakeTask(Task::Error,
                            "missing separator (did you mean TAB instead of 8 spaces?). Stop.",
                            Utils::FilePath::fromUserInput("Makefile"), 360))
            << QStringList();

        QTest::newRow("mingw32-make error")
            << QStringList()
            << QString::fromLatin1("mingw32-make[1]: *** [debug/qplotaxis.o] Error 1\n"
                                   "mingw32-make: *** [debug] Error 2")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << MakeTask(Task::Error,
                            "[debug/qplotaxis.o] Error 1"))
            << QStringList();

        QTest::newRow("mingw64-make error")
            << QStringList()
            << QString::fromLatin1("mingw64-make.exe[1]: *** [dynlib.inst] Error -1073741819")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << MakeTask(Task::Error,
                            "[dynlib.inst] Error -1073741819"))
            << QStringList();

        QTest::newRow("make warning")
            << QStringList()
            << QString::fromLatin1("make[2]: warning: jobserver unavailable: using -j1. Add `+' to parent make rule.")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << MakeTask(Task::Warning,
                            "jobserver unavailable: using -j1. Add `+' to parent make rule."))
            << QStringList();

        QTest::newRow("pass-through note")
            << QStringList()
            << QString::fromLatin1("/home/dev/creator/share/qtcreator/debugger/dumper.cpp:1079: note: initialized from here")
            << OutputParserTester::STDERR
            << QStringList() << QStringList("/home/dev/creator/share/qtcreator/debugger/dumper.cpp:1079: note: initialized from here")
            << Tasks()
            << QStringList();

        QTest::newRow("Full path make exe")
            << QStringList()
            << QString::fromLatin1("C:\\Qt\\4.6.2-Symbian\\s60sdk\\epoc32\\tools\\make.exe: *** [sis] Error 2")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << MakeTask(Task::Error,
                            "[sis] Error 2"))
            << QStringList();

        QTest::newRow("missing g++")
            << QStringList()
            << QString::fromLatin1("make: g++: Command not found")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << MakeTask(Task::Error,
                            "g++: Command not found"))
            << QStringList();

        QTest::newRow("warning in Makefile")
            << QStringList()
            << QString::fromLatin1("Makefile:794: warning: overriding commands for target `xxxx.app/Contents/Info.plist'")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << MakeTask(Task::Warning,
                            "overriding commands for target `xxxx.app/Contents/Info.plist'",
                            "Makefile", 794))
            << QStringList();
    }

    void testParsing()
    {
        OutputParserTester testbench;
        auto *childParser = new GnuMakeParser;
        auto *tester = new GnuMakeParserTester(childParser);
        connect(&testbench, &OutputParserTester::aboutToDeleteParser,
                tester, &GnuMakeParserTester::parserIsAboutToBeDeleted);

        testbench.addLineParser(childParser);
        QFETCH(QStringList, extraSearchDirs);
        QFETCH(QString, input);
        QFETCH(OutputParserTester::Channel, inputChannel);
        QFETCH(Tasks, tasks);
        QFETCH(QStringList, childStdOutLines);
        QFETCH(QStringList, childStdErrLines);
        QFETCH(QStringList, additionalSearchDirs);

        FilePaths searchDirs = childParser->searchDirectories();

        // add extra directories:
        for (const QString &dir : std::as_const(extraSearchDirs))
            testbench.addSearchDir(FilePath::fromString(dir));

        testbench.testParsing(input, inputChannel, tasks, childStdOutLines, childStdErrLines);

        // make sure we still have all the original dirs
        FilePaths newSearchDirs = tester->directories;
        for (const FilePath &dir : std::as_const(searchDirs)) {
            QVERIFY(newSearchDirs.contains(dir));
            newSearchDirs.removeOne(dir);
        }

        // make sure we have all additional dirs:
        for (const QString &dir : std::as_const(additionalSearchDirs)) {
            const FilePath fp = FilePath::fromString(dir);
            QVERIFY(newSearchDirs.contains(fp));
            newSearchDirs.removeOne(fp);
        }
        // make sure we have no extra cruft:
        QVERIFY(newSearchDirs.isEmpty());
        delete tester;
    }

    void testTaskMangling()
    {
        TemporaryFile theMakeFile("Makefile.XXXXXX");
        QVERIFY2(theMakeFile.open(), qPrintable(theMakeFile.errorString()));
        QFileInfo fi(theMakeFile);
        QVERIFY2(fi.fileName().startsWith("Makefile"), qPrintable(theMakeFile.filePath().path()));

        OutputParserTester testbench;
        auto *childParser = new GnuMakeParser;
        testbench.addLineParser(childParser);
        childParser->addSearchDir(FilePath::fromString(fi.absolutePath()));
        testbench.testParsing(
            fi.fileName() + ":360: *** missing separator (did you mean TAB instead of 8 spaces?). Stop.",
            OutputParserTester::STDERR,
            {MakeTask(Task::Error,
                      "missing separator (did you mean TAB instead of 8 spaces?). Stop.",
                      theMakeFile.filePath(), 360)},
            {}, {});
    }
};

QObject *createGnuMakeParserTest()
{
    return new GnuMakeParserTest;
}

} // namespace Internal

#endif // WITH_TESTS

} // namespace ProjectExplorer

#ifdef WITH_TESTS
#include <gnumakeparser.moc>
#endif
