// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "ioutputparser.h"

#include <QRegularExpression>
#include <QString>

namespace ProjectExplorer {

class PROJECTEXPLORER_EXPORT MsvcParser :  public ProjectExplorer::OutputTaskParser
{
public:
    MsvcParser();

    static Utils::Id id();

private:
    Result handleLine(const QString &line, Utils::OutputFormat type) override;
    bool isContinuation(const QString &line) const override;

    Result processCompileLine(const QString &line);

    QRegularExpression m_compileRegExp;
    QRegularExpression m_additionalInfoRegExp;
};

class PROJECTEXPLORER_EXPORT ClangClParser :  public ProjectExplorer::OutputTaskParser
{
public:
    ClangClParser();

private:
    Result handleLine(const QString &line, Utils::OutputFormat type) override;

    const QRegularExpression m_compileRegExp;
};

namespace Internal {
QObject *createMsvcParserTest();
QObject *createClangClParserTest();
}

} // namespace ProjectExplorer
