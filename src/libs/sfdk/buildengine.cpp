/****************************************************************************
**
** Copyright (C) 2012-2019 Jolla Ltd.
** Copyright (C) 2019-2020 Open Mobile Platform LLC.
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

#include "buildengine_p.h"

#include "dockervirtualmachine_p.h"
#include "sfdkconstants.h"
#include "sdk_p.h"
#include "targetsxmlreader_p.h"
#include "usersettings_p.h"
#include "virtualmachine_p.h"
#include "signingutils_p.h"

#include <ssh/sshconnection.h>
#include <ssh/sshremoteprocessrunner.h>
#include <utils/algorithm.h>
#include <utils/filesystemwatcher.h>
#include <utils/persistentsettings.h>
#include <utils/pointeralgorithm.h>
#include <utils/qtcassert.h>
#include <utils/qtcprocess.h>

#include <QDir>
#include <QElapsedTimer>
#include <QHostInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

using namespace QSsh;
using namespace Utils;

namespace Sfdk {

namespace {

const char DEFAULT_SNAPSHOT_SUFFIX[] = "default";
const char POOLED_SNAPSHOT_INFIX[] = ".pool.";
const char PROXY_CONFIG_FILE[] = "proxy.json";

const char* SIMPLE_WRAPPERS[] = {
    Constants::WRAPPER_CMAKE,
    Constants::WRAPPER_QMAKE,
    Constants::WRAPPER_MAKE,
    Constants::WRAPPER_GCC,
};

} // namespace anonymous

/*!
 * \class RpmValidationSuiteData
 */

bool RpmValidationSuiteData::operator==(const RpmValidationSuiteData &other) const
{
    return id == other.id
        && name == other.name
        && website == other.website
        && essential == other.essential;
}

/*!
 * \class BuildTargetData
 */

bool BuildTargetData::operator==(const BuildTargetData &other) const
{
    return name == other.name
        && origin == other.origin
        && sysRoot == other.sysRoot
        && toolsPath == other.toolsPath
        && gdb == other.gdb
        && rpmValidationSuites == other.rpmValidationSuites;
}

bool BuildTargetData::isValid() const
{
    return !name.isEmpty()
        && !sysRoot.isEmpty()
        && !toolsPath.isEmpty()
        && !gdb.isEmpty();
}

QString BuildTargetData::snapshotSuffix() const
{
    QTC_ASSERT(flags & Snapshot, return {});
    return name.mid(origin.length() + 1);
}

Utils::FilePath BuildTargetData::toolsPathCommonPrefix()
{
    return SdkPrivate::settingsLocation(SdkPrivate::UserScope)
        .pathAppended(Constants::BUILD_TARGET_TOOLS);
}

/*!
 * \class BuildEngine::PrivateConstructorTag
 * \internal
 */

struct BuildEngine::PrivateConstructorTag {};

/*!
 * \class BuildEngine
 */

BuildEngine::VmConnectionUiCreator BuildEngine::s_vmConnectionUiCreator;

BuildEngine::BuildEngine(QObject *parent, const PrivateConstructorTag &)
    : QObject(parent)
    , d_ptr(std::make_unique<BuildEnginePrivate>(this))
{
    Q_D(BuildEngine);

    d->creationTime = QDateTime::currentDateTime();
    d->wwwProxyType = Constants::WWW_PROXY_DISABLED;
}

BuildEngine::~BuildEngine() = default;

QUrl BuildEngine::uri() const
{
    return d_func()->virtualMachine->uri();
}

QString BuildEngine::name() const
{
    return d_func()->virtualMachine->name();
}

VirtualMachine *BuildEngine::virtualMachine() const
{
    return d_func()->virtualMachine.get();
}

bool BuildEngine::isAutodetected() const
{
    return d_func()->autodetected;
}

Utils::FilePath BuildEngine::sharedInstallPath() const
{
    return d_func()->sharedInstallPath;
}

Utils::FilePath BuildEngine::sharedHomePath() const
{
    return d_func()->sharedHomePath;
}

Utils::FilePath BuildEngine::sharedTargetsPath() const
{
    return d_func()->sharedTargetsPath;
}

Utils::FilePath BuildEngine::sharedConfigPath() const
{
    return d_func()->sharedConfigPath;
}

Utils::FilePath BuildEngine::sharedSrcPath() const
{
    return d_func()->sharedSrcPath;
}

QString BuildEngine::sharedSrcMountPoint() const
{
    return VirtualMachinePrivate::alignedMountPointFor(d_func()->sharedSrcPath.toString());
}

Utils::FilePath BuildEngine::sharedSshPath() const
{
    return d_func()->sharedSshPath;
}

void BuildEngine::setSharedSrcPath(const FilePath &sharedSrcPath, const QObject *context,
        const Functor<bool> &functor)
{
    QTC_CHECK(virtualMachine()->isLockedDown());

    const QPointer<const QObject> context_{context};

    VirtualMachinePrivate::get(virtualMachine())->setSharedPath(VirtualMachinePrivate::SharedSrc,
            sharedSrcPath, this, [=](bool ok) {
        if (ok)
            d_func()->setSharedSrcPath(sharedSrcPath);
        if (context_)
            functor(ok);
    });
}

quint16 BuildEngine::sshPort() const
{
    return d_func()->virtualMachine->sshParameters().port();
}

void BuildEngine::setSshPort(quint16 sshPort, const QObject *context, const Functor<bool> &functor)
{
    QTC_CHECK(virtualMachine()->isLockedDown());

    const QPointer<const QObject> context_{context};
    VirtualMachinePrivate::get(virtualMachine())->setReservedPortForwarding(
            VirtualMachinePrivate::SshPort, sshPort, this, [=](bool ok) {
        Q_D(BuildEngine);
        if (ok) {
            SshConnectionParameters sshParameters = d->virtualMachine->sshParameters();
            sshParameters.setPort(sshPort);
            d->setSshParameters(sshParameters);
        }
        if (context_)
            functor(ok);
    });
}

quint16 BuildEngine::dBusPort() const
{
    return d_func()->dBusPort;
}

void BuildEngine::setDBusPort(quint16 dBusPort, const QObject *context, const Functor<bool> &functor)
{
    QTC_CHECK(virtualMachine()->isLockedDown());

    const QPointer<const QObject> context_{context};
    VirtualMachinePrivate::get(virtualMachine())->setReservedPortForwarding(
            VirtualMachinePrivate::DBusPort, dBusPort, this, [=](bool ok) {
        if (ok)
            d_func()->setDBusPort(dBusPort);
        if (context_)
            functor(ok);
    });
}

Utils::FilePath BuildEngine::dBusNonceFilePath() const
{
    const FilePath nonceDirPath = SdkPrivate::cacheLocation()
        .pathAppended(Constants::BUILD_ENGINE_DBUS_NONCE_DIR);
    return nonceDirPath.pathAppended(name().replace(':', '_'));
}

QString BuildEngine::wwwProxyType() const
{
    return d_func()->wwwProxyType;
}

QString BuildEngine::wwwProxyServers() const
{
    return d_func()->wwwProxyServers;
}

QString BuildEngine::wwwProxyExcludes() const
{
    return d_func()->wwwProxyExcludes;
}

void BuildEngine::setWwwProxy(const QString &type, const QString &servers, const QString &excludes)
{
    Q_D(BuildEngine);
    // FIXME Introduce an enum for proxy type
    QTC_ASSERT(!type.isEmpty(), return);

    if (type == d->wwwProxyType
            && servers == d->wwwProxyServers
            && excludes == d->wwwProxyExcludes) {
        return;
    }

    d->wwwProxyType = type;
    d->wwwProxyServers = servers;
    d->wwwProxyExcludes = excludes;

    d->syncWwwProxy();

    emit wwwProxyChanged(type, servers, excludes);
}

QStringList BuildEngine::buildTargetNames() const
{
    return Utils::transform(d_func()->buildTargetsData, &BuildTargetData::name);
}

QStringList BuildEngine::buildTargetOrigins() const
{
    QStringList origins = Utils::transform(d_func()->buildTargetsData, &BuildTargetData::origin);
    Utils::sort(origins);
    origins.erase(std::unique(origins.begin(), origins.end()), origins.end());
    origins.removeAll(QString());
    return origins;
}

QList<BuildTargetData> BuildEngine::buildTargets() const
{
    return d_func()->buildTargetsData;
}

BuildTargetData BuildEngine::buildTarget(const QString &name) const
{
    return Utils::findOrDefault(d_func()->buildTargetsData,
            Utils::equal(&BuildTargetData::name, name));
}

BuildTargetData BuildEngine::buildTargetByOrigin(const QString &origin, const QString &snapshotSuffix) const
{
    QTC_ASSERT(!snapshotSuffix.isEmpty() || snapshotSuffix.isNull(), return {});

    return Utils::findOrDefault(d_func()->buildTargetsData, [&](const BuildTargetData &target) {
        return target.flags & BuildTargetData::Snapshot
            && target.origin == origin
            && (!snapshotSuffix.isNull() || target.flags & BuildTargetData::DefaultSnapshot)
            && (snapshotSuffix.isNull() || target.snapshotSuffix() == snapshotSuffix);
    });
}

void BuildEngine::importPrivateGpgKey(const QString &id,
        const Utils::FilePath &passphraseFile,
        const QObject *context,
        const Functor<bool, QString> &functor)
{
    QString errorString;
    QTC_ASSERT(isGpgAvailable(&errorString), {
        BatchComposer::enqueueCheckPoint(context, std::bind(functor, false, errorString));
        return;
    });

    const FilePath sharedGpgDir = sharedConfigPath()
            .stringAppended(Constants::BUILD_ENGINE_HOST_GNUPG_PATH_POSTFIX);

    const QPointer<const QObject> context_{context};
    SigningUtils::exportSecretKey(id, passphraseFile, sharedGpgDir, this,
            [=](bool ok, const QString &errorString) {
        if (context_)
            functor(ok, errorString);
    });
}

/*!
 * \class BuildEnginePrivate
 */

BuildEnginePrivate::BuildEnginePrivate(BuildEngine *q)
    : q_ptr(q)
{
}

BuildEnginePrivate::~BuildEnginePrivate() = default;

QVariantMap BuildEnginePrivate::toMap() const
{
    QVariantMap data;

    data.insert(Constants::BUILD_ENGINE_VM_URI, virtualMachine->uri());
    data.insert(Constants::BUILD_ENGINE_CREATION_TIME, creationTime);
    data.insert(Constants::BUILD_ENGINE_AUTODETECTED, autodetected);

    data.insert(Constants::BUILD_ENGINE_SHARED_INSTALL, sharedInstallPath.toString());
    data.insert(Constants::BUILD_ENGINE_SHARED_HOME, sharedHomePath.toString());
    data.insert(Constants::BUILD_ENGINE_SHARED_TARGET, sharedTargetsPath.toString());
    data.insert(Constants::BUILD_ENGINE_SHARED_CONFIG, sharedConfigPath.toString());
    data.insert(Constants::BUILD_ENGINE_SHARED_SRC, sharedSrcPath.toString());
    data.insert(Constants::BUILD_ENGINE_SHARED_SSH, sharedSshPath.toString());

    const SshConnectionParameters sshParameters = virtualMachine->sshParameters();
    data.insert(Constants::BUILD_ENGINE_HOST, sshParameters.host());
    data.insert(Constants::BUILD_ENGINE_USER_NAME, sshParameters.userName());
    data.insert(Constants::BUILD_ENGINE_PRIVATE_KEY_FILE, sshParameters.privateKeyFile);
    data.insert(Constants::BUILD_ENGINE_SSH_PORT, sshParameters.port());
    data.insert(Constants::BUILD_ENGINE_SSH_TIMEOUT, sshParameters.timeout);

    data.insert(Constants::BUILD_ENGINE_WWW_PROXY_TYPE, wwwProxyType);
    data.insert(Constants::BUILD_ENGINE_WWW_PROXY_SERVERS, wwwProxyServers);
    data.insert(Constants::BUILD_ENGINE_WWW_PROXY_EXCLUDES, wwwProxyExcludes);

    data.insert(Constants::BUILD_ENGINE_DBUS_PORT, dBusPort);
    data.insert(Constants::BUILD_ENGINE_HEADLESS, virtualMachine->isHeadless());

    int count = 0;
    for (const BuildTargetDump &target : buildTargets) {
        const QString key = QString(Constants::BUILD_ENGINE_TARGET_DATA_KEY_PREFIX)
            + QString::number(count);
        QVariantMap targetData = target.toMap();
        QTC_ASSERT(!targetData.isEmpty(), return {});
        data.insert(key, targetData);
        ++count;
    }
    data.insert(Constants::BUILD_ENGINE_TARGETS_COUNT_KEY, count);

    return data;
}

bool BuildEnginePrivate::fromMap(const QVariantMap &data)
{
    Q_Q(BuildEngine);

    const QUrl vmUri = data.value(Constants::BUILD_ENGINE_VM_URI).toUrl();
    QTC_ASSERT(vmUri.isValid(), return false);
    QTC_ASSERT(!virtualMachine || virtualMachine->uri() == vmUri, return false);

    if (!virtualMachine) {
        if (!initVirtualMachine(vmUri))
            return false;
    }

    creationTime = data.value(Constants::BUILD_ENGINE_CREATION_TIME).toDateTime();
    autodetected = data.value(Constants::BUILD_ENGINE_AUTODETECTED).toBool();

    auto toFilePath = [](const QVariant &v) { return FilePath::fromString(v.toString()); };
    setSharedInstallPath(toFilePath(data.value(Constants::BUILD_ENGINE_SHARED_INSTALL)));
    setSharedHomePath(toFilePath(data.value(Constants::BUILD_ENGINE_SHARED_HOME)));
    setSharedTargetsPath(toFilePath(data.value(Constants::BUILD_ENGINE_SHARED_TARGET)));
    setSharedConfigPath(toFilePath(data.value(Constants::BUILD_ENGINE_SHARED_CONFIG)));
    setSharedSrcPath(toFilePath(data.value(Constants::BUILD_ENGINE_SHARED_SRC)));
    setSharedSshPath(toFilePath(data.value(Constants::BUILD_ENGINE_SHARED_SSH)));

    SshConnectionParameters sshParameters = virtualMachine->sshParameters();
    sshParameters.setHost(data.value(Constants::BUILD_ENGINE_HOST).toString());
    sshParameters.setUserName(data.value(Constants::BUILD_ENGINE_USER_NAME).toString());
    sshParameters.privateKeyFile = data.value(Constants::BUILD_ENGINE_PRIVATE_KEY_FILE).toString();
    sshParameters.setPort(data.value(Constants::BUILD_ENGINE_SSH_PORT).toUInt());
    sshParameters.timeout = data.value(Constants::BUILD_ENGINE_SSH_TIMEOUT).toInt();
    if (sshParameters.timeout == 0)
        sshParameters.timeout = Constants::BUILD_ENGINE_DEFAULT_SSH_TIMEOUT;
    setSshParameters(sshParameters);

    setDBusPort(data.value(Constants::BUILD_ENGINE_DBUS_PORT).toUInt());

    q->setWwwProxy(data.value(Constants::BUILD_ENGINE_WWW_PROXY_TYPE,
                Constants::WWW_PROXY_DISABLED).toString(),
            data.value(Constants::BUILD_ENGINE_WWW_PROXY_SERVERS).toString(),
            data.value(Constants::BUILD_ENGINE_WWW_PROXY_EXCLUDES).toString());

    if (virtualMachine->features() & VirtualMachine::OptionalHeadless) {
        const bool headless = data.value(Constants::BUILD_ENGINE_HEADLESS).toBool();
        virtualMachine->setHeadless(headless);
    }

    const int newCount = data.value(Constants::BUILD_ENGINE_TARGETS_COUNT_KEY).toInt();
    QList<BuildTargetDump> newBuildTargets;
    for (int i = 0; i < newCount; ++i) {
        const QString key = QString(Constants::BUILD_ENGINE_TARGET_DATA_KEY_PREFIX)
            + QString::number(i);
        QTC_ASSERT(data.contains(key), return false);
        const QVariantMap targetData = data.value(key).toMap();
        BuildTargetDump target;
        // FIXME handle error?
        target.fromMap(targetData);
        newBuildTargets.append(target);
    }
    updateBuildTargets(newBuildTargets);

    return true;
}

bool BuildEnginePrivate::initVirtualMachine(const QUrl &vmUri)
{
    Q_Q(BuildEngine);
    Q_ASSERT(!virtualMachine);
    QTC_ASSERT(BuildEngine::s_vmConnectionUiCreator, return false);

    VirtualMachine::Features unsupportedFeatures = VirtualMachine::Snapshots;

    virtualMachine = VirtualMachineFactory::create(vmUri, ~unsupportedFeatures,
            BuildEngine::s_vmConnectionUiCreator());
    QTC_ASSERT(virtualMachine, return false);

    SshConnectionParameters sshParameters;
    sshParameters.setUserName(Constants::BUILD_ENGINE_DEFAULT_USER_NAME);
    sshParameters.setHost(Constants::BUILD_ENGINE_DEFAULT_HOST);
    sshParameters.timeout = Constants::BUILD_ENGINE_DEFAULT_SSH_TIMEOUT;
    sshParameters.hostKeyCheckingMode = SshHostKeyCheckingNone;
    sshParameters.authenticationType = SshConnectionParameters::AuthenticationTypeSpecificKey;
    sshParameters.forwardAgent = true;
    virtualMachine->setSshParameters(sshParameters);

    QObject::connect(VirtualMachinePrivate::get(virtualMachine.get()),
            QOverload<>::of(&VirtualMachinePrivate::prepare),
            q,
            [=]() { prepare(); });

    QObject::connect(VirtualMachinePrivate::get(virtualMachine.get()),
            QOverload<>::of(&VirtualMachinePrivate::initGuest),
            q,
            [=]() { initGuest(); });

    return true;
}

void BuildEnginePrivate::enableUpdates()
{
    Q_Q(BuildEngine);
    QTC_ASSERT(SdkPrivate::isVersionedSettingsEnabled(), return);
    QTC_ASSERT(!targetsXmlWatcher, return);

    if (SdkPrivate::enableVmAutoConnectInitially())
        virtualMachine->setAutoConnectEnabled(true);

    updateVmProperties(q, [](bool ok) { Q_UNUSED(ok) });

    targetsXmlWatcher = std::make_unique<FileSystemWatcher>(q);
    targetsXmlWatcher->addFile(targetsXmlFile().toString(), FileSystemWatcher::WatchModifiedDate);
    QObject::connect(Sdk::instance(), &Sdk::aboutToShutDown, q,
            [this]() { targetsXmlWatcher.reset(); });
    QObject::connect(targetsXmlWatcher.get(), &FileSystemWatcher::fileChanged,
            [this]() { updateBuildTargets(); });
    updateBuildTargets();
}

void BuildEnginePrivate::updateOnce()
{
    QTC_ASSERT(!SdkPrivate::isVersionedSettingsEnabled(), return);

    if (SdkPrivate::enableVmAutoConnectInitially())
        virtualMachine->setAutoConnectEnabled(true);

    // FIXME Not ideal
    bool ok;
    execAsynchronous(std::tie(ok), std::mem_fn(&BuildEnginePrivate::updateVmProperties), this);
    QTC_CHECK(ok);

    updateBuildTargets();
}

void BuildEnginePrivate::updateVmProperties(const QObject *context, const Functor<bool> &functor)
{
    Q_Q(BuildEngine);

    const QPointer<const QObject> context_{context};

    virtualMachine->refreshConfiguration(q, [=](bool ok) {
        if (!ok) {
            if (context_)
                functor(false);
            return;
        }

        const VirtualMachineInfo info =
            VirtualMachinePrivate::get(virtualMachine.get())->cachedInfo();

        setSharedInstallPath(FilePath::fromString(info.sharedInstall));
        setSharedHomePath(FilePath::fromString(info.sharedHome));
        setSharedTargetsPath(FilePath::fromString(info.sharedTargets));
        // FIXME if sharedConfig changes, at least privateKeyFile path needs to be updated
        setSharedConfigPath(FilePath::fromString(info.sharedConfig));
        setSharedSrcPath(FilePath::fromString(info.sharedSrc));
        setSharedSshPath(FilePath::fromString(info.sharedSsh));

        SshConnectionParameters sshParameters = virtualMachine->sshParameters();
        sshParameters.setPort(info.sshPort);
        setSshParameters(sshParameters);

        setDBusPort(info.dBusPort);

        if (context_)
            functor(true);
    });
}

bool BuildEnginePrivate::isValid() const
{
    QTC_ASSERT(!sharedInstallPath.isEmpty(), return false);
    QTC_ASSERT(!sharedHomePath.isEmpty(), return false);
    QTC_ASSERT(!sharedTargetsPath.isEmpty(), return false);
    QTC_ASSERT(!sharedConfigPath.isEmpty(), return false);
    QTC_ASSERT(!sharedSrcPath.isEmpty(), return false);
    QTC_ASSERT(!sharedSshPath.isEmpty(), return false);
    QTC_ASSERT(!virtualMachine->sshParameters().host().isEmpty(), return false);
    QTC_ASSERT(!virtualMachine->sshParameters().userName().isEmpty(), return false);
    QTC_ASSERT(virtualMachine->sshParameters().port(), return false);
    QTC_ASSERT(dBusPort, return false);
    return true;
}

void BuildEnginePrivate::setSharedInstallPath(const Utils::FilePath &sharedInstallPath)
{
    if (this->sharedInstallPath == sharedInstallPath)
        return;
    this->sharedInstallPath = sharedInstallPath;
    emit q_func()->sharedInstallPathChanged(sharedInstallPath);
}

void BuildEnginePrivate::setSharedHomePath(const Utils::FilePath &sharedHomePath)
{
    if (this->sharedHomePath == sharedHomePath)
        return;
    this->sharedHomePath = sharedHomePath;
    emit q_func()->sharedHomePathChanged(sharedHomePath);
}

void BuildEnginePrivate::setSharedTargetsPath(const Utils::FilePath &sharedTargetsPath)
{
    if (this->sharedTargetsPath == sharedTargetsPath)
        return;
    this->sharedTargetsPath = sharedTargetsPath;
    emit q_func()->sharedTargetsPathChanged(sharedTargetsPath);
}

void BuildEnginePrivate::setSharedConfigPath(const Utils::FilePath &sharedConfigPath)
{
    if (this->sharedConfigPath == sharedConfigPath)
        return;
    this->sharedConfigPath = sharedConfigPath;
    emit q_func()->sharedConfigPathChanged(sharedConfigPath);

    SshConnectionParameters sshParameters = virtualMachine->sshParameters();
    // FIXME hardcoded
    sshParameters.privateKeyFile = sharedConfigPath.pathAppended("/ssh/private_keys/engine")
        .pathAppended(Constants::BUILD_ENGINE_DEFAULT_USER_NAME).toString();
    setSshParameters(sshParameters);
}

void BuildEnginePrivate::setSharedSrcPath(const Utils::FilePath &sharedSrcPath)
{
    if (this->sharedSrcPath == sharedSrcPath)
        return;
    this->sharedSrcPath = sharedSrcPath;
    emit q_func()->sharedSrcPathChanged(sharedSrcPath);
    emit q_func()->sharedSrcMountPointChanged(q_func()->sharedSrcMountPoint());
}

void BuildEnginePrivate::setSharedSshPath(const Utils::FilePath &sharedSshPath)
{
    if (this->sharedSshPath == sharedSshPath)
        return;
    this->sharedSshPath = sharedSshPath;
    emit q_func()->sharedSshPathChanged(sharedSshPath);
}

void BuildEnginePrivate::setSshParameters(const QSsh::SshConnectionParameters &sshParameters)
{
    Q_Q(BuildEngine);

    const SshConnectionParameters old = virtualMachine->sshParameters();
    virtualMachine->setSshParameters(sshParameters);
    if (sshParameters.port() != old.port())
        emit q->sshPortChanged(sshParameters.port());
}

void BuildEnginePrivate::setDBusPort(quint16 dBusPort)
{
    if (this->dBusPort == dBusPort)
        return;
    this->dBusPort = dBusPort;
    emit q_func()->dBusPortChanged(dBusPort);
}

void BuildEnginePrivate::prepare()
{
    syncWwwProxy();
}

void BuildEnginePrivate::initGuest()
{
    fetchDBusNonce();
}

void BuildEnginePrivate::syncWwwProxy()
{
    const QRegularExpression spaces("[[:space:]]+");
    const QStringList wwwProxyServers = this->wwwProxyServers.split(spaces, Qt::SkipEmptyParts);
    const QStringList wwwProxyExcludes = this->wwwProxyExcludes.split(spaces, Qt::SkipEmptyParts);

    QVariantMap config;
    config["Method"] = wwwProxyType;

    if (!wwwProxyServers.isEmpty()
            && wwwProxyType == Constants::WWW_PROXY_AUTOMATIC) {
        // Proper messaging should be done on the UI side
        if (wwwProxyServers.count() > 1) {
            qCDebug(engine) << "Multiple proxy servers specified."
                << "Using just the first one for auto configuration";
        }
        config["URL"] = wwwProxyServers.first();
    }

    if (!wwwProxyServers.isEmpty()
            && wwwProxyType == Constants::WWW_PROXY_MANUAL) {
        config["Servers"] = wwwProxyServers;
    }

    if (!wwwProxyExcludes.isEmpty()
            && wwwProxyType == Constants::WWW_PROXY_MANUAL) {
        config["Excludes"] = wwwProxyExcludes;
    }

    const FilePath configPath = sharedConfigPath.pathAppended(PROXY_CONFIG_FILE);
    FileSaver saver(configPath.toString(), QIODevice::WriteOnly);
    saver.write(QJsonDocument::fromVariant(config).toJson());
    const bool ok = saver.finalize();
    QTC_ASSERT(ok, qCCritical(engine) << saver.errorString());
}

void BuildEnginePrivate::fetchDBusNonce()
{
    Q_Q(BuildEngine);

    const QString script(R"(
        sudo cat /run/sdk-setup/sfdk_bus_nonce
    )");

    auto runner = std::make_unique<RemoteProcessRunner>("fetch-dbus-nonce",
            script, virtualMachine->sshParameters());
    QObject::connect(runner.get(), &CommandRunner::success, q, [=, sshRunner = runner->sshRunner()]() {
        saveDBusNonce(sshRunner->readAllStandardOutput());
    });

    BatchComposer::enqueue(std::move(runner));
}

void BuildEnginePrivate::saveDBusNonce(const QByteArray &nonce)
{
    Q_Q(BuildEngine);

    if (nonce.length() != 16) {
        qCWarning(engine) << "Ignoring D-Bus nonce of unexpected length" << nonce.length();
        return;
    }

    const FilePath nonceFilePath = q->dBusNonceFilePath();
    const FilePath nonceDirPath = nonceFilePath.parentDir();
    if (!QDir().mkpath(nonceDirPath.toString())) {
        qCWarning(engine) << "Failed to create D-Bus nonce directory"
            << nonceDirPath.toString();
        return;
    }

    const QFileInfo nonceDirInfo(nonceDirPath.toString());
    if (!nonceDirInfo.isDir()) {
        qCWarning(engine) << "File is not a directory:" << nonceDirPath.toString();
        return;
    }

    const QFileDevice::Permissions privateDirPermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    if (nonceDirInfo.permissions() != privateDirPermissions
            && !QFile::setPermissions(nonceDirPath.toString(), privateDirPermissions)) {
        qCWarning(engine) << "Failed to set D-Bus nonce directory permissions:"
            << nonceDirPath.toString();
        return;
    }

    FileSaver nonceSaver(nonceFilePath.toString());
    nonceSaver.write(nonce);
    if (!nonceSaver.finalize()) {
        qCWarning(engine) << "Failed to write D-Bus nonce file" << nonceFilePath.toString()
            << ":" << nonceSaver.errorString();
    }
}

void BuildEnginePrivate::updateBuildTargets()
{
    Q_Q(BuildEngine);
    const QString targetsXml = targetsXmlFile().toString();
    qCDebug(engine) << "Updating build targets for" << q->uri().toString() << "from" << targetsXml;
    TargetsXmlReader reader(targetsXml);
    QTC_ASSERT(!reader.hasError(), {
        qCDebug(engine).noquote() << "Error reading targets.xml:" << reader.errorString();
        return;
    });
    QTC_ASSERT(reader.version() == 4, return);
    updateBuildTargets(reader.targets());
}

void BuildEnginePrivate::updateBuildTargets(QList<BuildTargetDump> newTargets)
{
    Q_Q(BuildEngine);

    // Sanity check
    const QStringList origins = Utils::transform(newTargets, &BuildTargetDump::origin);
    newTargets = Utils::filtered(newTargets, [=](const BuildTargetDump &target) {
        const bool targetHasSnapshots = origins.contains(target.name);
        QTC_ASSERT(!targetHasSnapshots, {
            qCDebug(engine) << "Ignoring build target with snapshots:" << target.name;
            return false;
        });

        if (!target.origin.isEmpty()) {
            const bool snapshotNameStartsWithOriginName = target.name.startsWith(target.origin + '.')
                && target.name.length() > target.origin.length() + 1;
            QTC_ASSERT(snapshotNameStartsWithOriginName, {
                qCDebug(engine) << "Ignoring badly named build target snapshot:" << target.name;
                return false;
            });
        }

        return true;
    });

    QList<BuildTargetData> newTargetsData = Utils::transform(newTargets,
            std::bind(&BuildEnginePrivate::createTargetData, this, std::placeholders::_1));

    QList<int> toRemove;

    for (int i = 0; i < buildTargets.count(); ++i) {
        const int match = newTargets.indexOf(buildTargets.at(i));
        if (match != -1) {
            newTargets.removeAt(match);
            newTargetsData.removeAt(match);
            continue;
        }

        const int dataMatch = newTargetsData.indexOf(buildTargetsData.at(i));
        if (dataMatch != -1) {
            qCDebug(engine) << "Updating build target" << buildTargets.at(i).name;
            buildTargets[i] = newTargets[dataMatch];
            newTargets.removeAt(dataMatch);
            newTargetsData.removeAt(dataMatch);
            if (!SdkPrivate::useSystemSettingsOnly()) {
                deinitBuildTargetAt(i);
                initBuildTargetAt(i);
            }
            continue;
        }

        toRemove.append(i);
    }

    Utils::reverseForeach(toRemove, [=](int i) {
        qCDebug(engine) << "Removing build target" << buildTargets.at(i).name;
        emit q->aboutToRemoveBuildTarget(i);
        if (!SdkPrivate::useSystemSettingsOnly())
            deinitBuildTargetAt(i);
        buildTargets.removeAt(i);
        buildTargetsData.removeAt(i);
    });

    for (const BuildTargetDump &target : qAsConst(newTargets)) {
        qCDebug(engine) << "Adding build target" << target.name;
        buildTargets.append(target);
        buildTargetsData.append(createTargetData(target));
        if (!SdkPrivate::useSystemSettingsOnly())
            initBuildTargetAt(buildTargets.count() - 1);
        emit q->buildTargetAdded(buildTargets.count() - 1);
    }
}

BuildTargetData BuildEnginePrivate::createTargetData(const BuildTargetDump &targetDump) const
{
    BuildTargetData data;

    data.name = targetDump.name;
    data.origin = targetDump.origin;

    if (!data.origin.isEmpty()) {
        data.flags |= BuildTargetData::Snapshot;
        if (data.snapshotSuffix() == DEFAULT_SNAPSHOT_SUFFIX)
            data.flags |= BuildTargetData::DefaultSnapshot;
        if (data.snapshotSuffix().contains(POOLED_SNAPSHOT_INFIX))
            data.flags |= BuildTargetData::PooledSnapshot;
    }

    data.machine = targetDump.gccDumpMachine;
    data.sysRoot = sysRootForTarget(data.name);
    data.toolsPath = toolsPathForTarget(data.name);
    data.gdb = FilePath::fromString(Constants::DEFAULT_DEBUGGER_FILENAME);
    data.rpmValidationSuites = rpmValidationSuitesFromString(targetDump.rpmValidationSuites);

    return data;
}

QList<RpmValidationSuiteData> BuildEnginePrivate::rpmValidationSuitesFromString(
        const QString &string)
{
    QList<RpmValidationSuiteData> retv;
    QString string_(string);
    QTextStream in(&string_);
    QString line;
    while (in.readLineInto(&line)) {
        RpmValidationSuiteData data;

        QStringList split = line.split(' ', Qt::SkipEmptyParts);
        if (split.count() < 3) {
            qCWarning(engine) << "Error parsing listing of RPM validation suites: The corrupted line is:" << line;
            break;
        }

        data.id = split.takeFirst();
        data.essential = split.takeFirst().toLower() == "essential";
        data.website = split.takeFirst();
        if (data.website == "-")
            data.website.clear();
        data.name = split.join(' ');

        retv.append(data);
    }

    return retv;
}

QString BuildEnginePrivate::rpmValidationSuitesToString(const QList<RpmValidationSuiteData> &suites)
{
    QString retv;
    QTextStream out(&retv);
    for (const auto &suite : suites) {
        out << suite.id << ' '
            << (suite.essential ? "Essential " : "Optional ")
            << (suite.website.isEmpty() ? QString("-") : suite.website) << ' '
            << suite.name
            << endl;
    }
    return retv;
}

void BuildEnginePrivate::initBuildTargetAt(int index) const
{
    QTC_ASSERT(!SdkPrivate::useSystemSettingsOnly(), return);

    const BuildTargetDump *dump = &buildTargets[index];
    const BuildTargetData *data = &buildTargetsData[index];

    const FilePath toolsPath = toolsPathForTarget(data->name);

    QDir toolsDir(toolsPath.toString());
    if (toolsDir.exists()) {
        qCDebug(engine) << "Not overwritting existing tools under" << toolsPath;
        return;
    }

    const bool mkpathOk = toolsDir.mkpath(".");
    QTC_ASSERT(mkpathOk, return);

    QString patchedQmakeQuery = dump->qmakeQuery;
    patchedQmakeQuery.replace(":/",
            sysRootForTarget(dump->name).fileNameWithPathComponents(-1).prepend(':').append('/'));

    QString patchedGccDumpIncludes = dump->gccDumpIncludes;
    patchedGccDumpIncludes.replace(" /",
            sysRootForTarget(dump->name).fileNameWithPathComponents(-1).prepend(' ').append('/'));

    QString patchedGccDumpInstallDir = dump->gccDumpInstallDir;
    patchedGccDumpInstallDir.replace(" /",
            sysRootForTarget(dump->name).fileNameWithPathComponents(-1).prepend(' ').append('/'));
    auto cacheFile = [=](const QString &baseName) {
        return toolsPath.pathAppended(baseName);
    };

    bool ok = true;

    ok &= createCacheFile(cacheFile(Constants::QMAKE_QUERY_CACHE), patchedQmakeQuery);
    ok &= createCacheFile(cacheFile(Constants::CMAKE_CAPABILITIES_CACHE), dump->cmakeCapabilities);
    ok &= createCacheFile(cacheFile(Constants::CMAKE_VERSION_CACHE), dump->cmakeVersion);
    ok &= createCacheFile(cacheFile(Constants::GCC_DUMP_MACHINE_CACHE), dump->gccDumpMachine);
    ok &= createCacheFile(cacheFile(Constants::GCC_DUMP_MACROS_CACHE), dump->gccDumpMacros);
    ok &= createCacheFile(cacheFile(Constants::GCC_DUMP_INCLUDES_CACHE), patchedGccDumpIncludes);
    ok &= createCacheFile(cacheFile(Constants::GCC_DUMP_INSTALL_DIR_CACHE), patchedGccDumpInstallDir);

    QTC_ASSERT(ok, return);

    for (const char *const wrapperName : SIMPLE_WRAPPERS)
        ok &= createSimpleWrapper(toolsPath, QString::fromLatin1(wrapperName));

    ok &= createPkgConfigWrapper(toolsPath, sysRootForTarget(dump->name));

    QTC_CHECK(ok);
}

void BuildEnginePrivate::deinitBuildTargetAt(int index) const
{
    QTC_ASSERT(!SdkPrivate::useSystemSettingsOnly(), return);
    QTC_ASSERT(index < buildTargets.count(), return);
    FileUtils::removeRecursively(toolsPathForTarget(buildTargets.at(index).name));
}

bool BuildEnginePrivate::createCacheFile(const FilePath &filePath, const QString &data) const
{
    FileSaver saver(filePath.toString(), QIODevice::WriteOnly);
    saver.write(data.toUtf8());
    if (!data.endsWith('\n'))
        saver.write("\n");
    const bool ok = saver.finalize();
    QTC_ASSERT(ok, qCCritical(engine) << saver.errorString(); return false);
    return true;
}

bool BuildEnginePrivate::createSimpleWrapper(const FilePath &toolsPath,
        const QString &wrapperName) const
{
    const QString commandName = HostOsInfo::isWindowsHost()
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        ? wrapperName.chopped(4) // remove the ".cmd"
#else
        ? [=] { auto chopped = wrapperName; chopped.chop(4); return chopped; }()
#endif
        : wrapperName;

    const QString wrapperBinaryPath = SdkPrivate::libexecPath()
        .pathAppended("merssh").stringAppended(QTC_HOST_EXE_SUFFIX).toString();

    const QString scriptCopyPath = toolsPath.pathAppended(wrapperName).toString();

    QString scriptContent;
    if (HostOsInfo::isWindowsHost()) {
        scriptContent = QStringLiteral(R"(@echo off
SetLocal EnableDelayedExpansion
set ARGUMENTS=
FOR %%a IN (%*) DO set ARGUMENTS=!ARGUMENTS! ^ '%%a'
set {MER_SSH_SDK_TOOLS}={toolsPath}
SetLocal DisableDelayedExpansion
"{wrapperBinaryPath}" {commandName} %ARGUMENTS%
)");
    } else {
        scriptContent = QStringLiteral(R"(#!/bin/sh
ARGUMENTS=""
for ARGUMENT in "$@"; do
    ARGUMENTS="${ARGUMENTS} '${ARGUMENT}'"
done
export {MER_SSH_SDK_TOOLS}="{toolsPath}"
exec "{wrapperBinaryPath}" {commandName} ${ARGUMENTS}
)");
    }

    scriptContent
        .replace("{MER_SSH_SDK_TOOLS}", Constants::MER_SSH_SDK_TOOLS)
        .replace("{toolsPath}", QDir::toNativeSeparators(toolsPath.toString()))
        .replace("{wrapperBinaryPath}", QDir::toNativeSeparators(wrapperBinaryPath))
        .replace("{commandName}", commandName);

    bool ok;

    FileSaver saver(scriptCopyPath, QIODevice::WriteOnly);
    saver.write(scriptContent.toUtf8());
    ok = saver.finalize();
    QTC_ASSERT(ok, qCCritical(engine) << saver.errorString(); return false);

    QFileInfo info(scriptCopyPath);
    ok = QFile::setPermissions(scriptCopyPath,
            info.permissions() | QFile::ExeOwner | QFile::ExeUser | QFile::ExeGroup);
    QTC_ASSERT(ok, return false);

    return ok;
}

bool BuildEnginePrivate::createPkgConfigWrapper(const FilePath &toolsPath, const FilePath &sysRoot)
    const
{
    auto nativeSysRooted = [=](const QString &path) {
        return QDir::toNativeSeparators(sysRoot.pathAppended(path).toString());
    };

    QStringList libDirs = {"/usr/lib64/pkgconfig", "/usr/lib/pkgconfig", "/usr/share/pkgconfig"};
    libDirs = Utils::transform(libDirs, nativeSysRooted);
    libDirs = Utils::filtered(libDirs, QOverload<const QString &>::of(QFileInfo::exists));

    const QString fileName = toolsPath.pathAppended(Constants::WRAPPER_PKG_CONFIG).toString();

    QString scriptContent;
    if (HostOsInfo::isWindowsHost()) {
        const QString real = QDir::toNativeSeparators(SdkPrivate::libexecPath()
                    .pathAppended("pkg-config.exe").toString());
        scriptContent = QStringLiteral(R"(@echo off
set PKG_CONFIG_DIR=
set PKG_CONFIG_LIBDIR={libDir}
REM NB, with pkg-config 0.26-1 on Windows it does not work with PKG_CONFIG_SYSROOT_DIR set
set PKG_CONFIG_SYSROOT_DIR=
{real} %*
)");
        scriptContent.replace("{real}", real);
        scriptContent.replace("{libDir}", libDirs.join(QDir::listSeparator()));
    } else {
        scriptContent = QStringLiteral(R"(#!/bin/sh
export PKG_CONFIG_DIR=
export PKG_CONFIG_LIBDIR="{libDir}"
export PKG_CONFIG_SYSROOT_DIR="{sysRoot}"
# It's useless to say anything here, qmake discards stderr
real=$(which -a pkg-config |sed -n 2p)
exec ${real?} "$@"
)");
        scriptContent.replace("{libDir}", libDirs.join(QDir::listSeparator()));
        scriptContent.replace("{sysRoot}", sysRoot.toString());
    }

    bool ok;

    FileSaver saver(fileName, QIODevice::WriteOnly);
    saver.write(scriptContent.toUtf8());
    ok = saver.finalize();
    QTC_ASSERT(ok, qCCritical(engine) << saver.errorString(); return false);

    QFileInfo info(fileName);
    ok = QFile::setPermissions(fileName,
            info.permissions() | QFile::ExeOwner | QFile::ExeUser | QFile::ExeGroup);
    QTC_ASSERT(ok, return false);

    return true;
}

Utils::FilePath BuildEnginePrivate::targetsXmlFile() const
{
    // FIXME
    return sharedTargetsPath.pathAppended("targets.xml");
}

Utils::FilePath BuildEnginePrivate::sysRootForTarget(const QString &targetName) const
{
    // FIXME inside MerTarget::finalizeKitCreation FilePath::fromUserInput was used in this context
    return sharedTargetsPath.pathAppended(targetName);
}

Utils::FilePath BuildEnginePrivate::toolsPathForTarget(const QString &targetName) const
{
    return BuildTargetData::toolsPathCommonPrefix()
        .pathAppended(virtualMachine->name().replace(':', '_'))
        .pathAppended(targetName);
}

/*!
 * \class BuildTargetDump
 */

bool BuildTargetDump::operator==(const BuildTargetDump &other) const
{
    return name == other.name
        && origin == other.origin
        && gccDumpMachine == other.gccDumpMachine
        && gccDumpMacros == other.gccDumpMacros
        && gccDumpIncludes == other.gccDumpIncludes
        && gccDumpInstallDir == other.gccDumpInstallDir
        && qmakeQuery == other.qmakeQuery
        && cmakeCapabilities == other.cmakeCapabilities
        && cmakeVersion == other.cmakeVersion
        && rpmValidationSuites == other.rpmValidationSuites;
}

QVariantMap BuildTargetDump::toMap() const
{
    QVariantMap data;
    data.insert(Constants::BUILD_TARGET_NAME, name);
    data.insert(Constants::BUILD_TARGET_ORIGIN, origin);
    data.insert(Constants::BUILD_TARGET_GCC_DUMP_MACHINE, gccDumpMachine);
    data.insert(Constants::BUILD_TARGET_GCC_DUMP_MACROS, gccDumpMacros);
    data.insert(Constants::BUILD_TARGET_GCC_DUMP_INCLUDES, gccDumpIncludes);
    data.insert(Constants::BUILD_TARGET_GCC_DUMP_INSTALL_DIR, gccDumpInstallDir);
    data.insert(Constants::BUILD_TARGET_QMAKE_QUERY, qmakeQuery);
    data.insert(Constants::BUILD_TARGET_CMAKE_CAPABILITIES, cmakeCapabilities);
    data.insert(Constants::BUILD_TARGET_CMAKE_VERSION, cmakeVersion);
    data.insert(Constants::BUILD_TARGET_RPM_VALIDATION_SUITES, rpmValidationSuites);
    return data;
}

void BuildTargetDump::fromMap(const QVariantMap &data)
{
    name = data.value(Constants::BUILD_TARGET_NAME).toString();
    origin = data.value(Constants::BUILD_TARGET_ORIGIN).toString();
    gccDumpMachine = data.value(Constants::BUILD_TARGET_GCC_DUMP_MACHINE).toString();
    gccDumpMacros = data.value(Constants::BUILD_TARGET_GCC_DUMP_MACROS).toString();
    gccDumpIncludes = data.value(Constants::BUILD_TARGET_GCC_DUMP_INCLUDES).toString();
    gccDumpInstallDir = data.value(Constants::BUILD_TARGET_GCC_DUMP_INSTALL_DIR).toString();
    qmakeQuery = data.value(Constants::BUILD_TARGET_QMAKE_QUERY).toString();
    cmakeCapabilities = data.value(Constants::BUILD_TARGET_CMAKE_CAPABILITIES).toString();
    cmakeVersion = data.value(Constants::BUILD_TARGET_CMAKE_VERSION).toString();
    rpmValidationSuites = data.value(Constants::BUILD_TARGET_RPM_VALIDATION_SUITES).toString();
}

/*!
 * \class BuildEngineManager
 */

BuildEngineManager *BuildEngineManager::s_instance = nullptr;

BuildEngineManager::BuildEngineManager(QObject *parent)
    : QObject(parent)
    , m_userSettings(std::make_unique<UserSettings>(
                Constants::BUILD_ENGINES_SETTINGS_FILE_NAME,
                Constants::BUILD_ENGINES_SETTINGS_DOC_TYPE, this))
{
    Q_ASSERT(!s_instance);
    s_instance = this;

    QHostInfo::lookupHost(QHostInfo::localHostName(), this,
            &BuildEngineManager::completeHostNameLookup);

    if (!SdkPrivate::useSystemSettingsOnly()) {
        // FIXME ugly
        const optional<QVariantMap> userData = m_userSettings->load();
        if (userData)
            fromMap(userData.value());
    }

    if (SdkPrivate::isVersionedSettingsEnabled()) {
        connect(SdkPrivate::instance(), &SdkPrivate::enableUpdatesRequested,
                this, &BuildEngineManager::enableUpdates);
    } else {
        connect(SdkPrivate::instance(), &SdkPrivate::updateOnceRequested,
                this, &BuildEngineManager::updateOnce);
    }

    connect(SdkPrivate::instance(), &SdkPrivate::saveSettingsRequested,
            this, &BuildEngineManager::saveSettings);
}

BuildEngineManager::~BuildEngineManager()
{
    s_instance = nullptr;
}

BuildEngineManager *BuildEngineManager::instance()
{
    return s_instance;
}

QString BuildEngineManager::installDir()
{
    // Not initialized initially. See Sdk for comments.
    QTC_CHECK(!s_instance->m_installDir.isEmpty());
    return s_instance->m_installDir;
}

QString BuildEngineManager::defaultBuildHostName()
{
    if (!s_instance->m_defaultBuildHostName.isNull())
        return s_instance->m_defaultBuildHostName;

    // A non-blocking lookup was initiated during initialization, but it takes
    // longer than anticipated - let's be patient.

    QElapsedTimer timer;
    timer.start();

    // TODO singletons vs. const correctness
    s_instance->completeHostNameLookup(QHostInfo::fromName(QHostInfo::localHostName()));

    qCDebug(lib) << "Local host info lookup blocked for" << timer.elapsed() << "ms";

    return s_instance->m_defaultBuildHostName;
}

QString BuildEngineManager::effectiveBuildHostName()
{
    return s_instance->m_customBuildHostName.isNull()
        ? defaultBuildHostName()
        : s_instance->m_customBuildHostName;
}

QString BuildEngineManager::customBuildHostName()
{
    return s_instance->m_customBuildHostName;
}

void BuildEngineManager::setCustomBuildHostName(const QString &hostName)
{
    if (s_instance->m_customBuildHostName == hostName)
        return;

    QTC_CHECK(!hostName.isEmpty() || hostName.isNull());
    s_instance->m_customBuildHostName = hostName;
    emit s_instance->customBuildHostNameChanged(s_instance->m_customBuildHostName);
}

QStringList BuildEngineManager::buildEnvironmentFilter()
{
    return s_instance->m_buildEnvironmentFilter;
}

void BuildEngineManager::setBuildEnvironmentFilter(const QStringList &filter)
{
    if (s_instance->m_buildEnvironmentFilter == filter)
        return;

    s_instance->m_buildEnvironmentFilter = filter;
    emit s_instance->buildEnvironmentFilterChanged(s_instance->m_buildEnvironmentFilter);
}

QList<BuildEngine *> BuildEngineManager::buildEngines()
{
    return Utils::toRawPointer<QList>(s_instance->m_buildEngines);
}

BuildEngine *BuildEngineManager::buildEngine(const QUrl &uri)
{
    return Utils::findOrDefault(s_instance->m_buildEngines, Utils::equal(&BuildEngine::uri, uri));
}

void BuildEngineManager::createBuildEngine(const QUrl &virtualMachineUri, const QObject *context,
        const Functor<std::unique_ptr<BuildEngine> &&> &functor)
{
    // Needs to be captured by multiple lambdas and their copies
    auto engine = std::make_shared<std::unique_ptr<BuildEngine>>(
            std::make_unique<BuildEngine>(nullptr, BuildEngine::PrivateConstructorTag{}));

    auto engine_d = BuildEnginePrivate::get(engine->get());
    if (!engine_d->initVirtualMachine(virtualMachineUri)) {
        BatchComposer::enqueueCheckPoint(context, std::bind(functor, nullptr));
        return;
    }
    engine_d->updateVmProperties(context, [=](bool ok) {
        QTC_CHECK(ok);
        if (!ok || !engine_d->isValid()) {
            functor({});
            return;
        }

        functor(std::move(*engine));
    });
}

int BuildEngineManager::addBuildEngine(std::unique_ptr<BuildEngine> &&buildEngine)
{
    QTC_ASSERT(buildEngine, return -1);

    if (SdkPrivate::isVersionedSettingsEnabled()) {
        QTC_ASSERT(SdkPrivate::isUpdatesEnabled(), return -1);
        BuildEnginePrivate::get(buildEngine.get())->enableUpdates();
    } else {
        BuildEnginePrivate::get(buildEngine.get())->updateOnce();
    }

    return s_instance->doAddBuildEngine(std::move(buildEngine));
}

void BuildEngineManager::removeBuildEngine(const QUrl &uri)
{
    int index = Utils::indexOf(s_instance->m_buildEngines, Utils::equal(&BuildEngine::uri, uri));
    QTC_ASSERT(index >= 0, return);

    emit s_instance->aboutToRemoveBuildEngine(index);
    s_instance->m_buildEngines.erase(s_instance->m_buildEngines.cbegin() + index);
}

QVariantMap BuildEngineManager::toMap() const
{
    QVariantMap data;
    data.insert(Constants::BUILD_ENGINES_VERSION_KEY, 1);
    data.insert(Constants::BUILD_ENGINES_INSTALL_DIR_KEY, m_installDir);
    data.insert(Constants::BUILD_ENGINES_CUSTOM_BUILD_HOST_NAME_KEY, m_customBuildHostName);
    data.insert(Constants::BUILD_ENGINES_BUILD_ENVIRONMENT_FILTER_KEY, m_buildEnvironmentFilter);

    int count = 0;
    for (const auto &engine : m_buildEngines) {
        const QVariantMap engineData = BuildEnginePrivate::get(engine.get())->toMap();
        QTC_ASSERT(!engineData.isEmpty(), return {});
        data.insert(QString::fromLatin1(Constants::BUILD_ENGINES_DATA_KEY_PREFIX)
                + QString::number(count),
                engineData);
        ++count;
    }
    data.insert(Constants::BUILD_ENGINES_COUNT_KEY, count);

    return data;
}

void BuildEngineManager::fromMap(const QVariantMap &data, bool fromSystemSettings)
{
    const int version = data.value(Constants::BUILD_ENGINES_VERSION_KEY).toInt();
    QTC_ASSERT(version == 1, return);

    m_installDir = data.value(Constants::BUILD_ENGINES_INSTALL_DIR_KEY).toString();
    QTC_ASSERT(!m_installDir.isEmpty(), return);

    if (!fromSystemSettings || m_customBuildHostName.isNull()) {
        setCustomBuildHostName(data.value(Constants::BUILD_ENGINES_CUSTOM_BUILD_HOST_NAME_KEY)
                .toString());
    }

    if (!fromSystemSettings || m_buildEnvironmentFilter.isEmpty()) {
        setBuildEnvironmentFilter(data.value(Constants::BUILD_ENGINES_BUILD_ENVIRONMENT_FILTER_KEY)
                .toStringList());
    }

    const int newCount = data.value(Constants::BUILD_ENGINES_COUNT_KEY, 0).toInt();
    QMap<QUrl, QVariantMap> newEnginesData;
    for (int i = 0; i < newCount; ++i) {
        const QString key = QString::fromLatin1(Constants::BUILD_ENGINES_DATA_KEY_PREFIX)
            + QString::number(i);
        QTC_ASSERT(data.contains(key), return);

        const QVariantMap engineData = data.value(key).toMap();
        const QUrl vmUri = engineData.value(Constants::BUILD_ENGINE_VM_URI).toUrl();
        QTC_ASSERT(!vmUri.isEmpty(), return);

        newEnginesData.insert(vmUri, engineData);
    }

    QMap<QUrl, BuildEngine *> existingBuildEngines;
    for (auto it = m_buildEngines.cbegin(); it != m_buildEngines.cend(); ) {
        const bool autodetected = it->get()->isAutodetected();
        const QUrl vmUri = it->get()->virtualMachine()->uri();
        const QDateTime creationTime = BuildEnginePrivate::get(it->get())->creationTime_();
        QTC_CHECK(creationTime.isValid());
        const bool inNewData = newEnginesData.contains(vmUri)
            && newEnginesData.value(vmUri)
                .value(Constants::BUILD_ENGINE_CREATION_TIME).toDateTime() == creationTime;
        if (!inNewData && (!fromSystemSettings || autodetected)) {
            qCDebug(engine) << "Dropping build engine" << vmUri.toString();
            emit aboutToRemoveBuildEngine(it - m_buildEngines.cbegin());
            it = m_buildEngines.erase(it);
        } else if (autodetected && fromSystemSettings) {
            qCDebug(engine) << "Preserving user configuration of build engine" << vmUri.toString();
            QTC_CHECK(inNewData);
            newEnginesData.remove(vmUri);
            ++it;
        } else {
            existingBuildEngines.insert(it->get()->virtualMachine()->uri(), it->get());
            ++it;
        }
    }

    // Update existing/add new engines
    for (const QUrl &vmUri : newEnginesData.keys()) {
        const QVariantMap engineData = newEnginesData.value(vmUri);
        BuildEngine *engine = existingBuildEngines.value(vmUri);
        std::unique_ptr<BuildEngine> newEngine;
        if (!engine) {
            qCDebug(Log::engine) << "Adding build engine" << vmUri.toString();
            newEngine = std::make_unique<BuildEngine>(this, BuildEngine::PrivateConstructorTag{});
            engine = newEngine.get();
        } else {
            qCDebug(Log::engine) << "Updating build engine" << vmUri.toString();
        }

        QTC_ASSERT(!fromSystemSettings || newEngine || engine->isAutodetected(), return);
        const bool ok = BuildEnginePrivate::get(engine)->fromMap(engineData);
        QTC_ASSERT(ok, return);

        if (newEngine)
            doAddBuildEngine(std::move(newEngine));
    }
}

int BuildEngineManager::doAddBuildEngine(std::unique_ptr<BuildEngine> &&buildEngine)
{
    QTC_ASSERT(buildEngine, return -1);
    m_buildEngines.emplace_back(std::move(buildEngine));
    const int index = m_buildEngines.size() - 1;
    emit buildEngineAdded(index);
    return index;
}

void BuildEngineManager::enableUpdates()
{
    QTC_ASSERT(SdkPrivate::isVersionedSettingsEnabled(), return);

    qCDebug(engine) << "Enabling updates";

    connect(m_userSettings.get(), &UserSettings::updated,
            this, [=](const QVariantMap &data) { fromMap(data); });
    m_userSettings->enableUpdates();

    checkSystemSettings();

    for (const std::unique_ptr<BuildEngine> &engine : m_buildEngines)
        BuildEnginePrivate::get(engine.get())->enableUpdates();
}

void BuildEngineManager::updateOnce()
{
    QTC_ASSERT(!SdkPrivate::isVersionedSettingsEnabled(), return);

    checkSystemSettings();

    for (const std::unique_ptr<BuildEngine> &engine : m_buildEngines)
        BuildEnginePrivate::get(engine.get())->updateOnce();
}

void BuildEngineManager::checkSystemSettings()
{
    qCDebug(engine) << "Checking system-wide configuration file"
        << systemSettingsFile();

    PersistentSettingsReader systemReader;
    if (!systemReader.load(systemSettingsFile())) {
        qCCritical(engine) << "Failed to load system-wide build engine configuration";
        return;
    }

    const QVariantMap systemData = systemReader.restoreValues();

    const bool fromSystemSettings = true;
    fromMap(systemData, fromSystemSettings);
}

void BuildEngineManager::saveSettings(QStringList *errorStrings) const
{
    QString errorString;
    const bool ok = m_userSettings->save(toMap(), &errorString);
    if (!ok)
        errorStrings->append(errorString);
}

void BuildEngineManager::completeHostNameLookup(const QHostInfo &info)
{
    // Sometimes hostname is available while FQDN is not and defaults to the
    // less usefull localhost[.localdomain]
    QStringList candidates{info.hostName(), QHostInfo::localHostName()};
    candidates.removeAll(QString());
    candidates.removeAll(QStringLiteral("localhost"));
    candidates.removeAll(QStringLiteral("localhost.localdomain"));
    candidates.append(QStringLiteral("localhost.localdomain"));
    m_defaultBuildHostName = candidates.first();
    Q_ASSERT(!m_defaultBuildHostName.isEmpty());
}

FilePath BuildEngineManager::systemSettingsFile()
{
    return SdkPrivate::settingsFile(SdkPrivate::SystemScope,
            Constants::BUILD_ENGINES_SETTINGS_FILE_NAME);
}

} // namespace Sfdk
