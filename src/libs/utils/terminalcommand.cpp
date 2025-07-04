// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "terminalcommand.h"

#include "algorithm.h"
#include "environment.h"
#include "hostosinfo.h"
#include "qtcsettings.h"

namespace Utils {

TerminalCommand::TerminalCommand(const FilePath &command, const QString &openArgs,
                                 const QString &executeArgs, bool needsQuotes)
    : command(command)
    , openArgs(openArgs)
    , executeArgs(executeArgs)
    , needsQuotes(needsQuotes)
{
}

bool TerminalCommand::operator==(const TerminalCommand &other) const
{
    return other.command == command && other.openArgs == openArgs
           && other.executeArgs == executeArgs;
}

bool TerminalCommand::operator<(const TerminalCommand &other) const
{
    if (command == other.command) {
        if (openArgs == other.openArgs)
            return executeArgs < other.executeArgs;
        return openArgs < other.openArgs;
    }
    return command < other.command;
}

Q_GLOBAL_STATIC_WITH_ARGS(const QList<TerminalCommand>, knownTerminals, (
{
    {"x-terminal-emulator", "", "-e"},
    {"xdg-terminal", "", "", true},
    {"xterm", "", "-e"},
    {"aterm", "", "-e"},
    {"Eterm", "", "-e"},
    {"rxvt", "", "-e"},
    {"urxvt", "", "-e"},
    {"xfce4-terminal", "", "-x"},
    {"konsole", "--separate --workdir .", "-e"},
    {"gnome-terminal", "", "--"},
    {"terminator", "", "-e", true},
}));

TerminalCommand TerminalCommand::defaultTerminalEmulator()
{
    static TerminalCommand defaultTerm;

    if (defaultTerm.command.isEmpty()) {
        if (HostOsInfo::isMacHost()) {
            return {"Terminal.app", "", ""};
        } else if (HostOsInfo::isAnyUnixHost()) {
            defaultTerm = {"xterm", "", "-e"};
            const Environment env = Environment::systemEnvironment();
            for (const TerminalCommand &term : *knownTerminals) {
                const FilePath result = env.searchInPath(term.command.path());
                if (!result.isEmpty()) {
                    defaultTerm = {result, term.openArgs, term.executeArgs, term.needsQuotes};
                    break;
                }
            }
        }
    }

    return defaultTerm;
}

QList<TerminalCommand> TerminalCommand::availableTerminalEmulators()
{
    QList<TerminalCommand> result;

    if (HostOsInfo::isAnyUnixHost()) {
        const Environment env = Environment::systemEnvironment();
        for (const TerminalCommand &term : *knownTerminals) {
            const FilePath command = env.searchInPath(term.command.path());
            if (!command.isEmpty())
                result.push_back({command, term.openArgs, term.executeArgs});
        }
        // sort and put default terminal on top
        const TerminalCommand defaultTerm = defaultTerminalEmulator();
        result.removeAll(defaultTerm);
        sort(result);
        result.prepend(defaultTerm);
    }

    return result;
}

const char kTerminalVersion[] = "4.8";
const char kTerminalVersionKey[] = "General/Terminal/SettingsVersion";
const char kTerminalCommandKey[] = "General/Terminal/Command";
const char kTerminalOpenOptionsKey[] = "General/Terminal/OpenOptions";
const char kTerminalExecuteOptionsKey[] = "General/Terminal/ExecuteOptions";

TerminalCommand TerminalCommand::terminalEmulator()
{
    TerminalCommand cmd;
    QtcSettings &settings = Utils::userSettings();
    if (HostOsInfo::isAnyUnixHost() && settings.contains(kTerminalCommandKey)) {
        FilePath command = FilePath::fromSettings(settings.value(kTerminalCommandKey));

        // TODO Remove some time after Qt Creator 11
        // Work around Qt Creator <= 10 writing the default terminal to the settings.
        if (HostOsInfo::isMacHost() && command.endsWith("openTerminal.py"))
            command = FilePath::fromString("Terminal.app");

        const TerminalCommand knownCommand = Utils::findOrDefault(
            *knownTerminals(), [fileName = command.fileName()](const TerminalCommand &known) {
                return known.command.fileName() == fileName;
            });
        cmd = {command,
               settings.value(kTerminalOpenOptionsKey).toString(),
               settings.value(kTerminalExecuteOptionsKey).toString(),
               knownCommand.needsQuotes};
    } else {
        cmd = defaultTerminalEmulator();
    }

    // Special handling for the "terminator" application, which may not work when invoked
    // via a generic symlink. See QTCREATORBUG-32111.
    if (cmd.command.fileName() == "x-terminal-emulator") {
        const FilePath canonicalCommand = cmd.command.canonicalPath();
        if (canonicalCommand.fileName() == "terminator") {
            cmd.command = canonicalCommand;
            cmd.needsQuotes = true;
        }
    }

    return cmd;
}

void TerminalCommand::setTerminalEmulator(const TerminalCommand &term)
{
    if (HostOsInfo::isAnyUnixHost()) {
        QtcSettings &settings = Utils::userSettings();
        settings.setValue(kTerminalVersionKey, kTerminalVersion);
        if (term == defaultTerminalEmulator()) {
            settings.remove(kTerminalCommandKey);
            settings.remove(kTerminalOpenOptionsKey);
            settings.remove(kTerminalExecuteOptionsKey);
        } else {
            settings.setValue(kTerminalCommandKey, term.command.toSettings());
            settings.setValue(kTerminalOpenOptionsKey, term.openArgs);
            settings.setValue(kTerminalExecuteOptionsKey, term.executeArgs);
        }
    }
}

} // Utils
