// Copyright (C) 2024 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#ifdef WITH_TESTS

#include <QObject>

namespace ProjectExplorer { class ProjectExplorerPlugin; }

namespace ProjectExplorer::Internal {

class ProjectExplorerTest final : public QObject
{
    Q_OBJECT

private:
    friend class ::ProjectExplorer::ProjectExplorerPlugin;
};

} // ProjectExplorer::Internal

#endif // WITH_TESTS
