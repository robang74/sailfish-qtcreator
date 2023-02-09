/****************************************************************************
**
** Copyright (C) 2019 Open Mobile Platform LLC.
** Contact: http://jolla.com/
**
** This file is part of Qt Creator.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** Other Usage
**
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Digia.
**
****************************************************************************/

#include "script.h"

#include "dispatch.h"
#include "sfdkconstants.h"
#include "sfdkglobal.h"
#include "textutils.h"

#include <sfdk/buildengine.h>
#include <sfdk/device.h>
#include <sfdk/sdk.h>
#include <sfdk/sfdkconstants.h>

#include <utils/algorithm.h>
#include <utils/fileutils.h>
#include <utils/optional.h>
#include <utils/qtcassert.h>

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>

using namespace Sfdk;
using namespace Utils;

namespace Sfdk {

namespace {
const char JS_CONFIGURATION_EXTENSION_NAME[] = "configuration";
const char JS_BUILD_ENGINE_EXTENSION_NAME[] = "buildEngine";
const char JS_MODULE_EXTENSION_NAME[] = "module";
const char JS_UTILS_EXTENSION_NAME[] = "utils";
} // namespace anonymous

class JSUtilsExtension : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString sdkVariant READ sdkVariant)
    Q_PROPERTY(QString pathSeparator READ pathSeparator)

public:
    using QObject::QObject;

    Q_INVOKABLE QString sdkVariant() const
    {
        return Sdk::sdkVariant();
    }

    Q_INVOKABLE QString regExpEscape(const QString &string) const
    {
        return QRegularExpression::escape(string);
    }

    Q_INVOKABLE bool isDevice(const QString &deviceName) const
    {
        for (Device *const device : Sdk::devices()) {
            if (device->name() == deviceName)
                return true;
        }
        return false;
    }

    Q_INVOKABLE QString pathSeparator() const
    {
        return QDir::separator();
    }

    Q_INVOKABLE bool exists(const QString &fileName) const
    {
        return QFileInfo::exists(fileName);
    }

    Q_INVOKABLE bool isDirectory(const QString &fileName) const
    {
        return QFileInfo(fileName).isDir();
    }

    Q_INVOKABLE bool isFile(const QString &fileName) const
    {
        return QFileInfo(fileName).isFile();
    }

    Q_INVOKABLE bool isAbsolute(const QString &path) const
    {
        return QDir::isAbsolutePath(path);
    }

    Q_INVOKABLE QString canonicalPath(const QString &path) const
    {
        return QDir(path).canonicalPath();
    }

    Q_INVOKABLE QString cleanPath(const QString &path) const
    {
        return QDir::cleanPath(path);
    }

    Q_INVOKABLE QString findFile_wide(const QStringList &paths, int maxDepth, const QString &nameFilter) const
    {
        QStringList subdirs;

        for (const QString &path : paths) {
            QDir dir(path);
            for (const QFileInfo &entryInfo : dir.entryInfoList({nameFilter}, QDir::Files))
                return entryInfo.filePath();
            if (maxDepth == 0)
                continue;
            for (const QFileInfo &subdirEntryInfo : dir.entryInfoList(QDir::Dirs))
                subdirs.append(subdirEntryInfo.filePath());
        }

        if (subdirs.isEmpty())
            return {};

        return findFile_wide(subdirs, maxDepth - 1, nameFilter);
    }

    Q_INVOKABLE void updateFile(const QString &fileName, QJSValue filterCallback) const
    {
        // FIXME Tried to use "~" suffix but there was no backup file after that
        const QString backupFileName = fileName + ".raw";

        bool ok;

        if (QFile::exists(backupFileName)) {
            ok = QFile::remove(backupFileName);
            if (!ok) {
                qjsEngine(this)->throwError(tr("Failed to remove old backup file \"%1\"")
                        .arg(backupFileName));
                return;
            }
        }

        ok = QFile::rename(fileName, backupFileName);
        if (!ok) {
            qjsEngine(this)->throwError(tr("Could not back up file \"%1\" as \"%2\"")
                    .arg(fileName).arg(backupFileName));
            return;
        }

        FileReader reader;
        ok = reader.fetch(backupFileName);
        if (!ok) {
            qjsEngine(this)->throwError(reader.errorString());
            return;
        }

        const QString data = QString::fromUtf8(reader.data());

        const QJSValue result = filterCallback.call({data});
        if (result.isError()) {
            qjsEngine(this)->throwError(tr("Uncaught exception in filterCallback: \"%1\"")
                    .arg(result.toString()));
            return;
        }

        const QString filtered = result.toString();

        FileSaver saver(fileName);
        saver.write(filtered.toUtf8());
        ok = saver.finalize();
        if (!ok) {
            qjsEngine(this)->throwError(saver.errorString());
            return;
        }
    }

    Q_INVOKABLE QStringList expandCompacted(const QString &string) const
    {
        QStringList expanded;
        if (!Sfdk::expandCompacted(string, &expanded)) {
            qjsEngine(this)->throwError(tr("Invalid compacted option specification: \"%1\"")
                    .arg(string));
            return {};
        }

        return expanded;
    }
};

class JSConfigurationExtension : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    Q_INVOKABLE bool isOptionSet(const QString &optionName) const
    {
        return optionEffectiveState(optionName).has_value();
    }

    Q_INVOKABLE QString optionArgument(const QString &optionName) const
    {
        QTC_ASSERT(isOptionSet(optionName), return {});
        return optionEffectiveState(optionName)->argument();
    }

    Q_INVOKABLE QString effectiveTarget() const
    {
        QString errorMessage;
        const BuildTargetData target = SdkManager::configuredTarget(&errorMessage);
        if (!target.isValid()) {
            qjsEngine(this)->throwError(errorMessage);
            return {};
        }

        return target.name;
    }

private:
    Utils::optional<OptionEffectiveOccurence> optionEffectiveState(const QString &optionName) const
    {
        const Option *const option = Dispatcher::option(optionName);
        QTC_ASSERT(option, return {});
        return Configuration::effectiveState(option);
    }
};

class JSBuildEngineExtension : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString sharedInstallPath READ sharedInstallPath)
    Q_PROPERTY(QString sharedHomePath READ sharedHomePath)
    Q_PROPERTY(QString sharedTargetsPath READ sharedTargetsPath)
    Q_PROPERTY(QString sharedConfigPath READ sharedConfigPath)
    Q_PROPERTY(QString sharedSrcPath READ sharedSrcPath)
    Q_PROPERTY(QString sharedSshPath READ sharedSshPath)
    Q_PROPERTY(QString sharedInstallMountPoint READ sharedInstallMountPoint)
    Q_PROPERTY(QString sharedHomeMountPoint READ sharedHomeMountPoint)
    Q_PROPERTY(QString sharedTargetsMountPoint READ sharedTargetsMountPoint)
    Q_PROPERTY(QString sharedConfigMountPoint READ sharedConfigMountPoint)
    Q_PROPERTY(QString sharedSrcMountPoint READ sharedSrcMountPoint)
    Q_PROPERTY(QString sharedSshMountPoint READ sharedSshMountPoint)

public:
    using QObject::QObject;

    QString sharedInstallPath() const { return enginePath(&BuildEngine::sharedInstallPath); }
    QString sharedHomePath() const { return enginePath(&BuildEngine::sharedHomePath); }
    QString sharedTargetsPath() const { return enginePath(&BuildEngine::sharedTargetsPath); }
    QString sharedConfigPath() const { return enginePath(&BuildEngine::sharedConfigPath); }
    QString sharedSrcPath() const { return enginePath(&BuildEngine::sharedSrcPath); }
    QString sharedSshPath() const { return enginePath(&BuildEngine::sharedSshPath); }

    QString sharedInstallMountPoint() const
    { return Sfdk::Constants::BUILD_ENGINE_SHARED_INSTALL_MOUNT_POINT; }
    QString sharedHomeMountPoint() const
    { return Sfdk::Constants::BUILD_ENGINE_SHARED_HOME_MOUNT_POINT; }
    QString sharedTargetsMountPoint() const
    { return Sfdk::Constants::BUILD_ENGINE_SHARED_TARGET_MOUNT_POINT; }
    QString sharedConfigMountPoint() const
    { return Sfdk::Constants::BUILD_ENGINE_SHARED_CONFIG_MOUNT_POINT; }
    QString sharedSrcMountPoint() const
    {
        return withEngine([=](BuildEngine *engine) {
            return engine->sharedSrcMountPoint();
        });
    }
    QString sharedSshMountPoint() const
    { return Sfdk::Constants::BUILD_ENGINE_SHARED_SSH_MOUNT_POINT; }

    Q_INVOKABLE bool hasBuildTarget(const QString &buildTargetName) const
    {
        return withEngine([=](BuildEngine *engine) {
            // For the case that no-snapshot option is NOT used
            if (engine->buildTargetOrigins().contains(buildTargetName))
                return true;
            // For the case that no-snapshot option is used
            if (engine->buildTargetNames().contains(buildTargetName))
                return true;

            // Try harder (and slower), considering also targets which are
            // not synchronized to host.
            QList<ToolsInfo> infos;
            const SdkManager::ListToolsOptions options{SdkManager::InstalledTools,
                SdkManager::UserDefinedTools, SdkManager::SnapshotTools};
            if (!SdkManager::listTools(options, &infos))
                return false;

            return Utils::anyOf(infos, [=](const ToolsInfo &info) {
                return info.flags & ToolsInfo::Target
                    && info.name == buildTargetName;
            });
        });
    }

    Q_INVOKABLE QString buildTargetToolsPath(const QString &buildTargetName) const
    {
        return withEngine([=](BuildEngine *engine) {
            return engine->buildTarget(buildTargetName).toolsPath.toString();
        });
    }

    Q_INVOKABLE void importGpgKey(const QString &signingUser, const QString &signingPassphraseFile)
    {
        withEngine([=](BuildEngine *engine) {
            QString errorString;
            bool ok;
            execAsynchronous(std::tie(ok, errorString),
                    std::mem_fn(&BuildEngine::importPrivateGpgKey),
                    engine,
                    signingUser,
                    FilePath::fromString(signingPassphraseFile));
            if (!ok)
                qjsEngine(this)->throwError(errorString);
        });
    }

    Q_INVOKABLE void setEnableReversePathMapping(bool enable)
    {
        SdkManager::setEnableReversePathMapping(enable);
    }

private:
    QString enginePath(Utils::FilePath (BuildEngine::*getter)() const) const
    {
        return withEngine([=](BuildEngine *engine) {
            return (engine->*getter)().toString();
        });
    }

    template<typename Fn>
    auto withEngine(Fn fn) const -> decltype(fn(SdkManager::engine()))
    {
        auto default_t = Sfdk::default_t<decltype(fn(SdkManager::engine()))>;

        if (!SdkManager::hasEngine()) {
            qjsEngine(this)->throwError(SdkManager::noEngineFoundMessage());
            return default_t();
        }
        return fn(SdkManager::engine());
    }
};

} // namespace Sfdk

/*!
 * \class JSEngine
 */

JSEngine::JSEngine(QObject *parent)
    : QJSEngine(parent)
{
    installExtensions(QJSEngine::TranslationExtension | QJSEngine::ConsoleExtension);

    globalObject().setProperty(JS_UTILS_EXTENSION_NAME,
            newQObject(new JSUtilsExtension));
    globalObject().setProperty(JS_CONFIGURATION_EXTENSION_NAME,
            newQObject(new JSConfigurationExtension));
    globalObject().setProperty(JS_BUILD_ENGINE_EXTENSION_NAME,
            newQObject(new JSBuildEngineExtension));
}

QJSValue JSEngine::evaluate(const QString &program, const Module *context)
{
    const QString moduleExtensionFileName =
        QDir(context->path).filePath(Constants::MODULE_SCRIPT_EXTENTION_FILE);

    if (QFileInfo(moduleExtensionFileName).exists()) {
        const QJSValue moduleExtension = importModule(moduleExtensionFileName);
        if (moduleExtension.isError())
            return moduleExtension;
        globalObject().setProperty(JS_MODULE_EXTENSION_NAME, moduleExtension);
    } else {
        const bool deleteOk = globalObject().deleteProperty(JS_MODULE_EXTENSION_NAME);
        QTC_CHECK(deleteOk);
    }

    return QJSEngine::evaluate(program);
}

QJSValue JSEngine::call(const QString &functionName, const QJSValueList &args,
        const Module *context, TypeValidator returnTypeValidator)
{
    QJSValue function = evaluate(functionName, context);

    if (function.isError()) {
        const QString errorFileName = function.property("fileName").toString();
        const int errorLineNumber = function.property("lineNumber").toInt();
        qCCritical(sfdk) << "Error dereferencing" << functionName
            << "in the context of" << context->path << "module:"
            << errorFileName << ":" << errorLineNumber << ":" << function.toString();
        return newErrorObject(QJSValue::GenericError, tr("Internal error"));
    }

    if (!function.isCallable()) {
        qCCritical(sfdk) << "Error dereferencing" << functionName
            << "in the context of" << context->path << "module:"
            << "The result is not callable";
        return newErrorObject(QJSValue::GenericError, tr("Internal error"));
    }

    const QJSValue result = function.call(args);

    if (result.isError()) {
        const QString errorFileName = function.property("fileName").toString();
        const int errorLineNumber = function.property("lineNumber").toInt();
        qCCritical(sfdk) << "Error calling" << functionName
            << "in the context of" << context->path << "module:"
            << errorFileName << ":" << errorLineNumber << ":" << result.toString();
        return result;
    }

    QString errorString;
    if (returnTypeValidator && !returnTypeValidator(result, &errorString)) {
        qCCritical(sfdk) << "Error calling" << functionName
            << "in the context of" << context->path << "module:"
            << "Unexpected return value:" << errorString;
        return newErrorObject(QJSValue::GenericError, tr("Internal error"));
    }

    return result;
}

#include "script.moc"
