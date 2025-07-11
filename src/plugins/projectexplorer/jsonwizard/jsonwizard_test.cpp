// Copyright (C) 2017 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "jsonwizard_test.h"

#include "jsonwizardfactory.h"

#include <coreplugin/icore.h>

#include <QCheckBox>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QTest>
#include <QLineEdit>
#include <QListView>
#include <QScopedPointer>
#include <QTest>

using namespace Utils;

namespace ProjectExplorer::Internal {

static QJsonObject createWidget(const QString &type, const QString &nameSuffix, const QJsonObject &data)
{
    return QJsonObject{
        {"name", QJsonValue(nameSuffix + type)},
        {"type", type},
        {"trDisplayName", QJsonValue(nameSuffix + "DisplayName")},
        {"data", data}
    };
}

static QJsonObject createFieldPageJsonObject(const QJsonArray &widgets)
{
    return QJsonObject{
        {"name", "testpage"},
        {"trDisplayName", "mytestpage"},
        {"typeId", "Fields"},
        {"data", widgets}
    };
}

static QJsonObject createGeneralWizard(const QJsonObject &pages)
{
    return QJsonObject {
        {"category", "TestCategory"},
        {"enabled", true},
        {"id", "mytestwizard"},
        {"trDisplayName", "mytest"},
        {"trDisplayCategory", "mytestcategory"},
        {"trDescription", "this is a test wizard"},
        {"generators",
            QJsonObject{
                {"typeId", "File"},
                {"data",
                    QJsonObject{
                        {"source", "myFile.txt"}
                    }
                }
            }
        },
        {"pages", pages}
    };
}

static QCheckBox *findCheckBox(Wizard *wizard, const QString &objectName)
{
    return wizard->findChild<QCheckBox *>(objectName + "CheckBox");
}

static QLineEdit *findLineEdit(Wizard *wizard, const QString &objectName)
{
    return wizard->findChild<QLineEdit *>(objectName + "LineEdit");
}

static QComboBox *findComboBox(Wizard *wizard, const QString &objectName)
{
    return wizard->findChild<QComboBox *>(objectName + "ComboBox");
};

using FactoryPtr = std::unique_ptr<JsonWizardFactory, QScopedPointerDeleteLater>;

class JsonWizardTest : public QObject
{
    Q_OBJECT

private slots:
    void testEmptyWizard()
    {
        const QJsonObject wizard = createGeneralWizard(QJsonObject());
        const Result<JsonWizardFactory *> res =
            JsonWizardFactory::createWizardFactory(wizard.toVariantMap(), {});

        QVERIFY(!res);
        QCOMPARE(res.error(), "Page has no typeId set.");
    }

    void testEmptyPage()
    {
        const QJsonObject pages = createFieldPageJsonObject(QJsonArray());
        const QJsonObject wizard = createGeneralWizard(pages);
        const Result<JsonWizardFactory *> res =
            JsonWizardFactory::createWizardFactory(wizard.toVariantMap(), {});

        QVERIFY(!res);
        QCOMPARE(res.error(), "When parsing fields of page \"PE.Wizard.Page.Fields\": No fields found.");
    }

    void testUnusedKeyAtFields_data()
    {
        const QPair<QString, QJsonValue> wrongData = {"wrong", false};

        QTest::addColumn<QJsonObject>("wrongDataJsonObect");
        QTest::newRow("Label") << QJsonObject({{wrongData, {"trText", "someText"}}});
        QTest::newRow("Spacer") << QJsonObject({wrongData});
        QTest::newRow("LineEdit") << QJsonObject({wrongData});
        QTest::newRow("TextEdit") << QJsonObject({wrongData});
        QTest::newRow("PathChooser") << QJsonObject({wrongData});
        QTest::newRow("CheckBox") << QJsonObject({wrongData});
        QTest::newRow("ComboBox") << QJsonObject({{wrongData, {"items", QJsonArray()}}});
    }

    void testUnusedKeyAtFields()
    {
        QString fieldType(QString::fromLatin1(QTest::currentDataTag()));
        QFETCH(QJsonObject, wrongDataJsonObect);
        const QJsonObject pages = QJsonObject{
                                              {"name", "testpage"},
                                              {"trDisplayName", "mytestpage"},
                                              {"typeId", "Fields"},
                                              {"data", createWidget(fieldType, "WrongKey", wrongDataJsonObect)},
                                              };
        const QJsonObject wizard = createGeneralWizard(pages);

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("has unsupported keys: wrong"));
        const Result<JsonWizardFactory *> res =
            JsonWizardFactory::createWizardFactory(wizard.toVariantMap(), {});

        QVERIFY(res.has_value());

        FactoryPtr factory(*res);
        QVERIFY(factory);
    }

    void testCheckBox()
    {
        const QJsonArray widgets({
            createWidget("CheckBox", "Default", QJsonObject()),
            createWidget("CheckBox", "Checked", QJsonObject({{"checked", true}})),
            createWidget("CheckBox", "UnChecked", QJsonObject({{"checked", false}})),
            createWidget("CheckBox", "SpecialValueUnChecked", QJsonObject(
                                                                  {{"checked", false}, {"checkedValue", "SpecialCheckedValue"}, {"uncheckedValue", "SpecialUnCheckedValue"}})
                         ),
            createWidget("CheckBox", "SpecialValueChecked", QJsonObject(
                                                                {{"checked", true}, {"checkedValue", "SpecialCheckedValue"}, {"uncheckedValue", "SpecialUnCheckedValue"}})
                         )
        });
        const QJsonObject pages = createFieldPageJsonObject(widgets);
        const QJsonObject wizardObject = createGeneralWizard(pages);

        const Result<JsonWizardFactory *> res =
            JsonWizardFactory::createWizardFactory(wizardObject.toVariantMap(), {});

        QVERIFY(res.has_value());
        JsonWizardFactory *factory = *res;
        QVERIFY(factory);

        std::unique_ptr<Wizard> wizard{factory->runWizard({}, Id(), QVariantMap())};

        QVERIFY(!findCheckBox(wizard.get(), "Default")->isChecked());
        QCOMPARE(wizard->field("DefaultCheckBox"), QVariant(false));

        QVERIFY(findCheckBox(wizard.get(), "Checked")->isChecked());
        QCOMPARE(wizard->field("CheckedCheckBox"), QVariant(true));

        QVERIFY(!findCheckBox(wizard.get(), "UnChecked")->isChecked());
        QCOMPARE(wizard->field("UnCheckedCheckBox"), QVariant(false));

        QVERIFY(!findCheckBox(wizard.get(), "SpecialValueUnChecked")->isChecked());
        QCOMPARE(qPrintable(wizard->field("SpecialValueUnCheckedCheckBox").toString()), "SpecialUnCheckedValue");

        QVERIFY(findCheckBox(wizard.get(), "SpecialValueChecked")->isChecked());
        QCOMPARE(qPrintable(wizard->field("SpecialValueCheckedCheckBox").toString()), "SpecialCheckedValue");
    }

    void testLineEdit()
    {
        const QJsonArray widgets({
            createWidget("LineEdit", "Default", QJsonObject()),
            createWidget("LineEdit", "WithText", QJsonObject({{"trText", "some text"}}))
        });
        const QJsonObject pages = createFieldPageJsonObject(widgets);
        const QJsonObject wizardObject = createGeneralWizard(pages);

        const Result<JsonWizardFactory *> res =
            JsonWizardFactory::createWizardFactory(wizardObject.toVariantMap(), {});

        QVERIFY(res.has_value());
        const FactoryPtr factory(*res);
        QVERIFY(factory);

        std::unique_ptr<Wizard> wizard{factory->runWizard({}, Id(), QVariantMap())};
        QVERIFY(wizard);
        QVERIFY(findLineEdit(wizard.get(), "Default"));
        QVERIFY(findLineEdit(wizard.get(), "Default")->text().isEmpty());
        QCOMPARE(qPrintable(findLineEdit(wizard.get(), "WithText")->text()), "some text");

        QVERIFY(!wizard->page(0)->isComplete());
        findLineEdit(wizard.get(), "Default")->setText("enable isComplete");
        QVERIFY(wizard->page(0)->isComplete());
    }

    void testComboBox()
    {
        const QJsonArray items({"abc", "cde", "fgh"});
        QJsonObject disabledComboBoxObject = createWidget("ComboBox", "Disabled", QJsonObject({ {{"disabledIndex", 2}, {"items", items}} }));
        disabledComboBoxObject.insert("enabled", false);
        const QJsonArray widgets({
            createWidget("ComboBox", "Default", QJsonObject({ {{"items", items}} })),
            createWidget("ComboBox", "Index2", QJsonObject({ {{"index", 2}, {"items", items}} })),
            disabledComboBoxObject
        });

        const QJsonObject pages = createFieldPageJsonObject(widgets);
        const QJsonObject wizardObject = createGeneralWizard(pages);

        const Result<JsonWizardFactory *> res =
            JsonWizardFactory::createWizardFactory(wizardObject.toVariantMap(), {});

        QVERIFY(res.has_value());
        const FactoryPtr factory(*res);
        QVERIFY(factory);
        std::unique_ptr<Wizard> wizard{factory->runWizard({}, Id(), QVariantMap())};

        QComboBox *defaultComboBox = findComboBox(wizard.get(), "Default");
        QVERIFY(defaultComboBox);
        QCOMPARE(defaultComboBox->count(), items.count());
        QCOMPARE(qPrintable(defaultComboBox->currentText()), "abc");

        defaultComboBox->setCurrentIndex(2);
        QCOMPARE(qPrintable(defaultComboBox->currentText()), "fgh");

        QComboBox *index2ComboBox = findComboBox(wizard.get(), "Index2");
        QVERIFY(index2ComboBox);
        QCOMPARE(qPrintable(index2ComboBox->currentText()), "fgh");

        QComboBox *disabledComboBox = findComboBox(wizard.get(), "Disabled");
        QVERIFY(disabledComboBox);
        QCOMPARE(qPrintable(disabledComboBox->currentText()), "fgh");
    }

    void testIconList()
    {
        const auto iconInsideResource = [](const QString &relativePathToIcon) {
            return Core::ICore::resourcePath().resolvePath(relativePathToIcon).toUrlishString();
        };

        const QJsonArray items({
            QJsonObject{
                {"trKey", "item no1"},
                {"condition", true},
                {"icon", iconInsideResource("templates/wizards/global/lib.png")}
            },
            QJsonObject{
                {"trKey", "item no2"},
                {"condition", false},
                {"icon", "not_existing_path"}

            },
            QJsonObject{
                {"trKey", "item no3"},
                {"condition", true},
                {"trToolTip", "MyToolTip"},
                {"icon", iconInsideResource("templates/wizards/global/lib.png")}
            }
        });

        const QJsonArray widgets({
            createWidget("IconList", "Fancy", QJsonObject{{"index", -1}, {"items", items}})
        });

        const QJsonObject pages = createFieldPageJsonObject(widgets);
        const QJsonObject wizardObject = createGeneralWizard(pages);

        const Result<JsonWizardFactory *> res =
            JsonWizardFactory::createWizardFactory(wizardObject.toVariantMap(), {});

        QVERIFY(res.has_value());
        const FactoryPtr factory(*res);
        QVERIFY(factory);
        std::unique_ptr<Wizard> wizard{factory->runWizard({}, Id(), QVariantMap())};

        auto view = wizard->findChild<QListView *>("FancyIconList");
        QCOMPARE(view->model()->rowCount(), 2);
        QVERIFY(view->model()->index(0,0).data(Qt::DecorationRole).canConvert<QIcon>());
        QIcon icon = view->model()->index(0,0).data(Qt::DecorationRole).value<QIcon>();
        QVERIFY(!icon.isNull());
        QVERIFY(!wizard->page(0)->isComplete());
    }
};

QObject *createJsonWizardTest()
{
    return new JsonWizardTest;
}

} // ProjectExplorer::Internal

#include <jsonwizard_test.moc>
