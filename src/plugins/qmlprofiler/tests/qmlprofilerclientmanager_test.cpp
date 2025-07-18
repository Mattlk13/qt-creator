// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qmlprofilerclientmanager_test.h"
#include "fakedebugserver.h"

#include <utils/url.h>

#include <QTcpServer>
#include <QTcpSocket>
#include <QLocalSocket>
#include <QQmlDebuggingEnabler>

#include <QTest>
#include <QSignalSpy>

using namespace ProjectExplorer;

namespace QmlProfiler::Internal {

struct MessageHandler
{
    MessageHandler(QtMessageHandler handler)
    {
        defaultHandler = qInstallMessageHandler(handler);
    }

    ~MessageHandler()
    {
        qInstallMessageHandler(defaultHandler);
    }

    static QtMessageHandler defaultHandler;
};

QtMessageHandler MessageHandler::defaultHandler;

QmlProfilerClientManagerTest::QmlProfilerClientManagerTest()
{
    clientManager.setRetryInterval(10);
    clientManager.setMaximumRetries(10);
}

void QmlProfilerClientManagerTest::testConnectionFailure_data()
{
    QTest::addColumn<QmlProfilerModelManager *>("modelManager");
    QVarLengthArray<QmlProfilerModelManager *> modelManagers({nullptr, &modelManager});

    QTest::addColumn<QmlProfilerStateManager *>("stateManager");
    QVarLengthArray<QmlProfilerStateManager *> stateManagers({nullptr, &stateManager});

    QUrl localUrl = Utils::urlFromLocalHostAndFreePort();

    QTest::addColumn<QUrl>("serverUrl");
    const QVarLengthArray<QString> hosts({"", "/-/|\\-\\|/-", localUrl.host()});
    const QVarLengthArray<int> ports({-1, 5, localUrl.port()});
    const QVarLengthArray<QString> sockets({"", "/-/|\\-\\|/-",
                                            Utils::urlFromLocalSocket().path()});
    const QVarLengthArray<QString> schemes({"", Utils::urlSocketScheme(),
                                            Utils::urlTcpScheme()});

    for (QmlProfilerModelManager *modelManager : modelManagers) {
        for (QmlProfilerStateManager *stateManager : stateManagers) {
            for (const QString &host : hosts) {
                for (int port : ports) {
                    for (const QString &socket : sockets) {
                        for (const QString &scheme : schemes ) {
                            QUrl url;
                            url.setScheme(scheme);
                            url.setHost(host);
                            url.setPort(port);
                            url.setPath(socket);
                            QString tag = QString::fromLatin1("%1, %2, %3")
                                    .arg(QLatin1String(modelManager ? "modelManager" : "<null>"))
                                    .arg(QLatin1String(stateManager ? "stateManager" : "<null>"))
                                    .arg(url.toString());
                            QTest::newRow(tag.toLatin1().constData()) << modelManager
                                                                      << stateManager << url;
                        }
                    }
                }
            }
        }
    }
}

void softAssertMessageHandler(QtMsgType type, const QMessageLogContext &context,
                              const QString &message)
{
    if (type != QtDebugMsg || !message.startsWith("SOFT ASSERT: "))
        MessageHandler::defaultHandler(type, context, message);
}

void QmlProfilerClientManagerTest::testConnectionFailure()
{
    clientManager.setRetryInterval(1);
    clientManager.setMaximumRetries(2);
    // This triggers a lot of soft asserts. We test that it still doesn't crash and stays in a
    // consistent state.
    QByteArray fatalAsserts =  qgetenv("QTC_FATAL_ASSERTS");
    qunsetenv("QTC_FATAL_ASSERTS");
    MessageHandler handler(&softAssertMessageHandler);
    Q_UNUSED(handler)

    QFETCH(QmlProfilerModelManager *, modelManager);
    QFETCH(QmlProfilerStateManager *, stateManager);
    QFETCH(QUrl, serverUrl);

    QSignalSpy openedSpy(&clientManager, &QmlProfilerClientManager::connectionOpened);
    QSignalSpy closedSpy(&clientManager, &QmlProfilerClientManager::connectionClosed);
    QSignalSpy failedSpy(&clientManager, &QmlProfilerClientManager::connectionFailed);

    QVERIFY(!clientManager.isConnected());

    clientManager.setModelManager(modelManager);
    clientManager.setProfilerStateManager(stateManager);

    QVERIFY(!clientManager.isConnected());

    clientManager.setServer(serverUrl);
    clientManager.connectToServer();
    QTRY_COMPARE(failedSpy.count(), 1);
    QCOMPARE(closedSpy.count(), 0);
    QCOMPARE(openedSpy.count(), 0);
    QVERIFY(!clientManager.isConnected());

    clientManager.retryConnect();
    QTRY_COMPARE(failedSpy.count(), 2);
    QCOMPARE(closedSpy.count(), 0);
    QCOMPARE(openedSpy.count(), 0);
    QVERIFY(!clientManager.isConnected());

    clientManager.disconnectFromServer();

    qputenv("QTC_FATAL_ASSERTS", fatalAsserts);
    clientManager.setRetryInterval(10);
    clientManager.setMaximumRetries(10);
}

void QmlProfilerClientManagerTest::testUnresponsiveTcp()
{
    QSignalSpy openedSpy(&clientManager, &QmlProfilerClientManager::connectionOpened);
    QSignalSpy closedSpy(&clientManager, &QmlProfilerClientManager::connectionClosed);
    QSignalSpy failedSpy(&clientManager, &QmlProfilerClientManager::connectionFailed);

    QVERIFY(!clientManager.isConnected());

    clientManager.setProfilerStateManager(&stateManager);
    clientManager.setModelManager(&modelManager);

    QUrl serverUrl = Utils::urlFromLocalHostAndFreePort();

    QTcpServer server;
    server.listen(QHostAddress(serverUrl.host()), serverUrl.port());
    QSignalSpy connectionSpy(&server, &QTcpServer::newConnection);

    clientManager.setServer(serverUrl);
    clientManager.connectToServer();

    QTRY_VERIFY(connectionSpy.count() > 0);
    QTRY_COMPARE(failedSpy.count(), 1);
    QCOMPARE(openedSpy.count(), 0);
    QCOMPARE(closedSpy.count(), 0);
    QVERIFY(!clientManager.isConnected());

    clientManager.disconnectFromServer();
}

void QmlProfilerClientManagerTest::testUnresponsiveLocal()
{
    QSignalSpy openedSpy(&clientManager, &QmlProfilerClientManager::connectionOpened);
    QSignalSpy closedSpy(&clientManager, &QmlProfilerClientManager::connectionClosed);
    QSignalSpy failedSpy(&clientManager, &QmlProfilerClientManager::connectionFailed);

    QVERIFY(!clientManager.isConnected());

    clientManager.setProfilerStateManager(&stateManager);
    clientManager.setModelManager(&modelManager);

    QUrl socketUrl = Utils::urlFromLocalSocket();
    QLocalSocket socket;
    QSignalSpy connectionSpy(&socket, &QLocalSocket::connected);

    clientManager.setServer(socketUrl);
    clientManager.connectToServer();

    socket.connectToServer(socketUrl.path());
    QTRY_COMPARE(connectionSpy.count(), 1);
    QTRY_COMPARE(failedSpy.count(), 1);
    QCOMPARE(openedSpy.count(), 0);
    QCOMPARE(closedSpy.count(), 0);
    QVERIFY(!clientManager.isConnected());

    clientManager.disconnectFromServer();
}

void responsiveTestData()
{
    QTest::addColumn<quint32>("flushInterval");

    QTest::newRow("no flush") << 0u;
    QTest::newRow("flush")    << 1u;
}

void QmlProfilerClientManagerTest::testResponsiveTcp_data()
{
    responsiveTestData();
}

void QmlProfilerClientManagerTest::testResponsiveTcp()
{
    QFETCH(quint32, flushInterval);

    QUrl serverUrl = Utils::urlFromLocalHostAndFreePort();

    QSignalSpy openedSpy(&clientManager, &QmlProfilerClientManager::connectionOpened);
    QSignalSpy closedSpy(&clientManager, &QmlProfilerClientManager::connectionClosed);

    QVERIFY(!clientManager.isConnected());

    {
        QTcpServer server;
        QScopedPointer<QTcpSocket> socket;
        connect(&server, &QTcpServer::newConnection, [&server, &socket]() {
            socket.reset(server.nextPendingConnection());
            fakeDebugServer(socket.data());
        });

        server.listen(QHostAddress(serverUrl.host()), serverUrl.port());

        clientManager.setProfilerStateManager(&stateManager);
        clientManager.setModelManager(&modelManager);
        clientManager.setFlushInterval(flushInterval);

        connect(&clientManager, &QmlProfilerClientManager::connectionFailed,
                &clientManager, &QmlProfilerClientManager::retryConnect);

        clientManager.setServer(serverUrl);
        clientManager.connectToServer();

        QTRY_COMPARE(openedSpy.count(), 1);
        QCOMPARE(closedSpy.count(), 0);
        QVERIFY(clientManager.isConnected());

        // Do some nasty things and make sure it doesn't crash
        stateManager.setCurrentState(QmlProfilerStateManager::AppRunning);
        stateManager.setClientRecording(false);
        stateManager.setClientRecording(true);
        clientManager.clearBufferedData();
        stateManager.setCurrentState(QmlProfilerStateManager::AppStopRequested);

        QVERIFY(socket);
    }

    QTRY_COMPARE(closedSpy.count(), 1);
    QVERIFY(!clientManager.isConnected());

    disconnect(&clientManager, &QmlProfilerClientManager::connectionFailed,
               &clientManager, &QmlProfilerClientManager::retryConnect);

    stateManager.setCurrentState(QmlProfilerStateManager::Idle);
}

void QmlProfilerClientManagerTest::testResponsiveLocal_data()
{
    responsiveTestData();
}

void QmlProfilerClientManagerTest::testResponsiveLocal()
{
    QFETCH(quint32, flushInterval);

    QUrl socketUrl = Utils::urlFromLocalSocket();

    QSignalSpy openedSpy(&clientManager, &QmlProfilerClientManager::connectionOpened);
    QSignalSpy closedSpy(&clientManager, &QmlProfilerClientManager::connectionClosed);

    QVERIFY(!clientManager.isConnected());

    clientManager.setProfilerStateManager(&stateManager);
    clientManager.setModelManager(&modelManager);
    clientManager.setFlushInterval(flushInterval);

    connect(&clientManager, &QmlProfilerClientManager::connectionFailed,
            &clientManager, &QmlProfilerClientManager::retryConnect);

    clientManager.setServer(socketUrl);
    clientManager.connectToServer();

    {
        QScopedPointer<QLocalSocket> socket(new QLocalSocket(this));
        socket->connectToServer(socketUrl.path());
        QVERIFY(socket->isOpen());
        fakeDebugServer(socket.data());

        QTRY_COMPARE(openedSpy.count(), 1);
        QCOMPARE(closedSpy.count(), 0);
        QVERIFY(clientManager.isConnected());

        // Do some nasty things and make sure it doesn't crash
        stateManager.setCurrentState(QmlProfilerStateManager::AppRunning);
        stateManager.setClientRecording(false);
        stateManager.setClientRecording(true);
        clientManager.clearBufferedData();
        stateManager.setCurrentState(QmlProfilerStateManager::AppStopRequested);
    }

    QTRY_COMPARE(closedSpy.count(), 1);
    QVERIFY(!clientManager.isConnected());

    disconnect(&clientManager, &QmlProfilerClientManager::connectionFailed,
               &clientManager, &QmlProfilerClientManager::retryConnect);

    stateManager.setCurrentState(QmlProfilerStateManager::Idle);
}

void invalidHelloMessageHandler(QtMsgType type, const QMessageLogContext &context,
                                const QString &message)
{
    if (type != QtWarningMsg || message != "QML Debug Client: Invalid hello message")
        MessageHandler::defaultHandler(type, context, message);
}

void QmlProfilerClientManagerTest::testInvalidData()
{
    MessageHandler handler(&invalidHelloMessageHandler);
    Q_UNUSED(handler)

    QSignalSpy openedSpy(&clientManager, &QmlProfilerClientManager::connectionOpened);
    QSignalSpy closedSpy(&clientManager, &QmlProfilerClientManager::connectionClosed);
    QSignalSpy failedSpy(&clientManager, &QmlProfilerClientManager::connectionFailed);

    QVERIFY(!clientManager.isConnected());

    clientManager.setProfilerStateManager(&stateManager);
    clientManager.setModelManager(&modelManager);

    QUrl serverUrl = Utils::urlFromLocalHostAndFreePort();

    bool dataSent = false;
    QTcpServer server;
    connect(&server, &QTcpServer::newConnection, [&server, &dataSent](){
        QTcpSocket *socket = server.nextPendingConnection();

        // emulate packet protocol
        qint32 sendSize32 = 10;
        socket->write((char *)&sendSize32, sizeof(qint32));
        socket->write("----------------------- x -----------------------");

        dataSent = true;
    });

    server.listen(QHostAddress(serverUrl.host()), serverUrl.port());

    clientManager.setServer(serverUrl);
    clientManager.connectToServer();

    QTRY_VERIFY(dataSent);
    QTRY_COMPARE(failedSpy.count(), 1);
    QCOMPARE(openedSpy.count(), 0);
    QCOMPARE(closedSpy.count(), 0);
    QVERIFY(!clientManager.isConnected());

    clientManager.disconnectFromServer();
}

void QmlProfilerClientManagerTest::testStopRecording()
{
    QUrl socketUrl = Utils::urlFromLocalSocket();

    {
        QmlProfilerClientManager clientManager;
        clientManager.setRetryInterval(10);
        clientManager.setMaximumRetries(10);
        QSignalSpy openedSpy(&clientManager, &QmlProfilerClientManager::connectionOpened);
        QSignalSpy closedSpy(&clientManager, &QmlProfilerClientManager::connectionClosed);

        QVERIFY(!clientManager.isConnected());

        clientManager.setProfilerStateManager(&stateManager);
        clientManager.setModelManager(&modelManager);

        connect(&clientManager, &QmlProfilerClientManager::connectionFailed,
                &clientManager, &QmlProfilerClientManager::retryConnect);

        clientManager.setServer(socketUrl);
        clientManager.connectToServer();

        QScopedPointer<QLocalSocket> socket(new QLocalSocket(this));
        socket->connectToServer(socketUrl.path());
        QVERIFY(socket->isOpen());
        fakeDebugServer(socket.data());

        QTRY_COMPARE(openedSpy.count(), 1);
        QCOMPARE(closedSpy.count(), 0);
        QVERIFY(clientManager.isConnected());

        // We can't verify that it does anything useful, but at least it doesn't crash
        clientManager.stopRecording();
    }

    // Delete while still connected, for added fun
}

void QmlProfilerClientManagerTest::testConnectionDrop()
{
    QUrl socketUrl = Utils::urlFromLocalSocket();
    QmlProfilerClientManager clientManager;

    {
        clientManager.setRetryInterval(10);
        clientManager.setMaximumRetries(10);
        clientManager.setProfilerStateManager(&stateManager);
        clientManager.setModelManager(&modelManager);
        clientManager.setServer(socketUrl);
        clientManager.connectToServer();

        QScopedPointer<QLocalSocket> socket(new QLocalSocket(this));
        socket->connectToServer(socketUrl.path());
        QVERIFY(socket->isOpen());
        fakeDebugServer(socket.data());

        // Fake a trace. We want to test that this is reset when the connection drops.
        stateManager.setServerRecording(true);
        QTRY_VERIFY(clientManager.isConnected());
    }

    QTRY_VERIFY(!stateManager.serverRecording());
}

} // namespace QmlProfiler::Internal
