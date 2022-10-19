/****************************************************************************
**
** Copyright (C) 2019 Jolla Ltd.
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

#include "command.h"

#include "cmake.h"
#include "commandlineparser.h"
#include "configuration.h"
#include "debugger.h"
#include "dispatch.h"
#include "script.h"
#include "sfdkconstants.h"
#include "sfdkglobal.h"
#include "task.h"
#include "textutils.h"

#include <sfdk/buildengine.h>
#include <sfdk/device.h>
#include <sfdk/emulator.h>
#include <sfdk/sdk.h>
#include <sfdk/sfdkconstants.h>
#include <sfdk/virtualmachine.h>

#include <ssh/sshremoteprocessrunner.h>
#include <utils/algorithm.h>
#include <utils/fileutils.h>
#include <utils/qtcassert.h>
#include <utils/qtcprocess.h>

#include <QCommandLineParser>
#include <QDebug>
#include <QDirIterator>
#include <QRegularExpression>

using namespace Sfdk;
using namespace Utils;

namespace {

const char PROGRAM_KEY[] = "program";
const char INITIAL_ARGUMENTS_KEY[] = "initialArguments";
const char OMIT_SUBCOMMAND_KEY[] = "omitSubcommand";
const char OPTION_FORMATTER_KEY[] = "optionFormatter";

const char EMULATOR_DEVICE_MODEL[] = "device-model";
const char EMULATOR_ORIENTATION[] = "orientation";
const char EMULATOR_DOWNSCALE[] = "downscale";
// TODO Downscale 4x?
//const char EMULATOR_DOWNSCALE_FACTOR[] = "downscale.factor";
const char EMULATOR_SSH_PORT[] = "ssh.port";
const char EMULATOR_SSH_TIMEOUT[] = "ssh.timeout";

const char ENGINE_HOST_NAME[] = "host-name";
const char ENGINE_BUILD_ENVIRONMENT_FILTER[] = "environment.forward";
const char ENGINE_SSH_PORT[] = "ssh.port";
const char ENGINE_SSH_TIMEOUT[] = "ssh.timeout";
const char ENGINE_DBUS_PORT[] = "dbus.port";

const char WWW_PROXY_TYPE[] = "proxy";
const char WWW_PROXY_SERVERS[] = "proxy.servers";
const char WWW_PROXY_EXCLUDES[] = "proxy.excludes";

const char VM_MEMORY_SIZE_MB[] = "vm.memory-size";
const char VM_SWAP_SIZE_MB[] = "vm.swap-size";
const char VM_CPU_COUNT[] = "vm.cpu-count";
const char VM_STORAGE_SIZE_MB[] = "vm.storage-size";
const char VM_FREE_STORAGE_SIZE_MB[] = "vm.free-storage-size"; // intentionally undocumented, write-only

const char QT_CREATOR_DEPLOYMENT_TXT[] = "QtCreatorDeployment.txt";

} // namespace anonymous

namespace Sfdk {

/*!
 * \class PropertiesAccessor
 */

class PropertiesAccessor
{
    Q_DECLARE_TR_FUNCTIONS(Sfdk::PropertiesAccessor)

public:
    enum PrepareResult {
        Ignored = 0,
        Prepared,
        Failed,
    };

public:
    virtual QMap<QString, QString> get() const = 0;
    virtual PrepareResult prepareSet(const QString &name, const QString &value, bool *needsVmOff,
            QString *errorString) = 0;
    virtual bool canSet(QString *errorString) const { Q_UNUSED(errorString); return true; }
    virtual bool set() = 0;

protected:
    static bool parsePositiveInt(int *out, const QString &string, QString *errorString)
    {
        bool ok;
        *out = string.toInt(&ok);
        if (!ok || *out <= 0) {
            *errorString = tr("Positive integer expected: \"%1\"").arg(string);
            return false;
        }

        return true;
    }

    static bool parseNonNegativeInt(int *out, const QString &string, QString *errorString)
    {
        bool ok;
        *out = string.toInt(&ok);
        if (!ok || *out < 0) {
            *errorString = tr("Non-negative integer expected: \"%1\"").arg(string);
            return false;
        }

        return true;
    }

    static bool parsePortNumber(quint16 *out, const QString &string, QString *errorString)
    {
        int asInt;
        if (!parsePositiveInt(&asInt, string, errorString))
            return false;
        if (asInt > std::numeric_limits<quint16>::max()) {
            *errorString = valueTooBigMessage();
            return false;
        }
        if (asInt < 1024) {
            *errorString = tr("Privileged ports may not be used");
            return false;
        }

        *out = static_cast<quint16>(asInt);
        return true;
    }

    static bool parseBoolean(bool *out, const QString &string, QString *errorString)
    {
        return parseWithDictionary(booleanDictionary(), out, string, errorString);
    }

    static QString showBoolean(bool value)
    {
        return showWithDictionary(booleanDictionary(), value);
    }

    static bool parseOrientation(Qt::Orientation *out, const QString &string, QString *errorString)
    {
        return parseWithDictionary(orientationDictionary(), out, string, errorString);
    }

    static QString showOrientation(Qt::Orientation value)
    {
        return showWithDictionary(orientationDictionary(), value);
    }

    template<typename T>
    static bool parseWithDictionary(const QList<QPair<T, QString>> &dictionary, T *out,
            const QString &string, QString *errorString)
    {
        auto it = std::find_if(std::begin(dictionary), std::end(dictionary), [&](const auto &item) {
            return item.second == string;
        });

        if (it == std::end(dictionary)) {
            const QStringList expected = Utils::transform(dictionary, &QPair<T, QString>::second);
            *errorString = tr("One of %1 expected. Got: \"%2\"")
                .arg(expected.join("/"))
                .arg(string);
            return false;
        }

        *out = it->first;
        return true;
    }

    template<typename T>
    static QString showWithDictionary(const QList<QPair<T, QString>> &dictionary, const T &value)
    {
        auto it = std::find_if(std::begin(dictionary), std::end(dictionary), [&](const auto &item) {
            return item.first == value;
        });
        QTC_ASSERT(it != std::end(dictionary), return {});

        return it->second;
    }

    static QString valueTooBigMessage()
    {
        return tr("Value too big");
    }

    static QString valueCannotBeDecreasedMessage()
    {
        return tr("Value cannot be decreased");
    }

    static QString valueCannotBeIncreasedMessage()
    {
        return tr("Value cannot be increased");
    }

    static QString valueEmptyMessage()
    {
        return tr("Unexpected empty string");
    }

    static QString unknownPropertyMessage()
    {
        return tr("Unrecognized property");
    }

    static QString readOnlyPropertyMessage()
    {
        return tr("Read-only property");
    }

private:
    static QList<QPair<bool, QString>> booleanDictionary()
    {
        return {
            {true, "yes"},
            {false, "no"}
        };
    }

    static QList<QPair<Qt::Orientation, QString>> orientationDictionary()
    {
        return {
            {Qt::Vertical, "portrait"},
            {Qt::Horizontal, "landscape"},
        };
    }
};

/*!
 * \class SetPropertiesTask
 */

class SetPropertiesTask : public Task
{
    Q_OBJECT

public:
    SetPropertiesTask(std::unique_ptr<PropertiesAccessor> &&accessor,
            VirtualMachine *virtualMachine, const QString &stopVmMessage)
        : m_accessor(std::move(accessor))
        , m_virtualMachine(virtualMachine)
        , m_stopVmMessage(stopVmMessage)
    {
    }

    QMap<QString, QString> get() const
    {
        return m_accessor->get();
    }

    bool prepareSet(const QString &name, const QString &value, QString *errorString)
    {
        bool needsVmOff = false;

        if (m_accessor->prepareSet(name, value, &needsVmOff, errorString)
                != PropertiesAccessor::Prepared) {
            return false;
        }

        m_needsVmOff |= needsVmOff;
        return true;
    }

    bool set(QString *errorString)
    {
        if (!m_accessor->canSet(errorString))
            return false;

        started();

        bool ok = false;
        bool lockDownOk = false;

        if (m_needsVmOff) {
            if (!qEnvironmentVariableIntValue(Constants::SFDK_AUTO_STOP_VMS_ENV_VAR)
                    && SdkManager::isRunningReliably(m_virtualMachine)) {
                *errorString = m_stopVmMessage;
            } else {
                execAsynchronous(std::tie(lockDownOk),
                    std::mem_fn(&VirtualMachine::lockDown),
                    m_virtualMachine, true);
                QTC_CHECK(lockDownOk);
            }
        }

        if (!m_needsVmOff || lockDownOk) {
            ok = m_accessor->set();
            if (!ok)
                *errorString = tr("Failed to set some of the properties");
        }

        if (lockDownOk)
            m_virtualMachine->lockDown(false, this, IgnoreAsynchronousReturn<bool>);

        exited();

        return ok;
    }

protected:
    void beginTerminate() override
    {
        qerr() << tr("Wait please...");
        endTerminate(true);
    }

    void beginStop() override
    {
        endStop(true);
    }

    void beginContinue() override
    {
        endContinue(true);
    }

private:
    const std::unique_ptr<PropertiesAccessor> m_accessor;
    const QPointer<VirtualMachine> m_virtualMachine;
    const QString m_stopVmMessage;
    bool m_needsVmOff = false;
};

/*!
 * \class VirtualMachinePropertiesAccessor
 */

class VirtualMachinePropertiesAccessor : public PropertiesAccessor
{
public:
    VirtualMachinePropertiesAccessor(VirtualMachine *virtualMachine)
        : m_vm(virtualMachine)
    {
        m_memorySizeMb = m_vm->memorySizeMb();
        m_swapSizeMb = m_vm->swapSizeMb();
        m_cpuCount = m_vm->cpuCount();
        m_storageSizeMb = m_vm->storageSizeMb();
    }

    QMap<QString, QString> get() const override
    {
        QMap<QString, QString> values;
        values.insert(VM_MEMORY_SIZE_MB, QString::number(m_memorySizeMb));
        if (m_vm->features() & VirtualMachine::SwapMemory)
            values.insert(VM_SWAP_SIZE_MB, QString::number(m_swapSizeMb));
        values.insert(VM_CPU_COUNT, QString::number(m_cpuCount));
        values.insert(VM_STORAGE_SIZE_MB, QString::number(m_storageSizeMb));
        return values;
    }

    PrepareResult prepareSet(const QString &name, const QString &value, bool *needsVmOff,
            QString *errorString) override
    {
        *needsVmOff = true;

        if (name == VM_MEMORY_SIZE_MB) {
            if (!(m_vm->features() & VirtualMachine::LimitMemorySize)){
                *errorString = readOnlyPropertyMessage();
                return Failed;
            }
            if (!parsePositiveInt(&m_memorySizeMb, value, errorString))
                return Failed;
            if (m_memorySizeMb > VirtualMachine::availableMemorySizeMb()) {
                *errorString = valueTooBigMessage();
                return Failed;
            }
            return Prepared;
        } else if (name == VM_SWAP_SIZE_MB && (m_vm->features() & VirtualMachine::SwapMemory)) {
            if (!parseNonNegativeInt(&m_swapSizeMb, value, errorString))
                return Failed;
            if (m_swapSizeMb > m_storageSizeMb) {
                *errorString = valueTooBigMessage();
                return Failed;
            }
            return Prepared;
        } else if (name == VM_CPU_COUNT) {
            if (!(m_vm->features() & VirtualMachine::LimitMemorySize)){
                *errorString = readOnlyPropertyMessage();
                return Failed;
            }
            if (!parsePositiveInt(&m_cpuCount, value, errorString))
                return Failed;
            if (m_cpuCount > VirtualMachine::availableCpuCount()) {
                *errorString = valueTooBigMessage();
                return Failed;
            }
            return Prepared;
        } else if (name == VM_STORAGE_SIZE_MB) {
            if (!parsePositiveInt(&m_storageSizeMb, value, errorString))
                return Failed;
            if (m_storageSizeMb < m_vm->storageSizeMb()
                    && !(m_vm->features() & VirtualMachine::ShrinkStorageSize)) {
                *errorString = valueCannotBeDecreasedMessage();
                return Failed;
            }
            if (m_storageSizeMb > m_vm->storageSizeMb()
                    && !(m_vm->features() & VirtualMachine::GrowStorageSize)) {
                *errorString = valueCannotBeIncreasedMessage();
                return Failed;
            }
            return Prepared;
        } else if (name == VM_FREE_STORAGE_SIZE_MB) {
            int commaIndex = value.indexOf(',');
            QString freeSize = value.left(commaIndex);
            if (freeSize.endsWith('+')) {
                freeSize.chop(1);
            } else {
                *errorString = valueCannotBeDecreasedMessage();
                return Failed;
            }
            if (!parsePositiveInt(&m_freeSizeMb, freeSize, errorString))
                return Failed;
            if (commaIndex > 0) {
                if (!parsePositiveInt(&m_incrementMb, value.mid(commaIndex + 1), errorString))
                    return Failed;
                if (m_incrementMb < m_freeSizeMb) {
                    *errorString = tr("<step> cannot be smaller than <size>");
                    return Failed;
                }
            }
            int currentFreeSizeMb = m_vm->freeStorageSizeMb();
            if (currentFreeSizeMb < 0) {
                *errorString = tr("Failed to determine current free storage size");
                return Failed;
            }
            if (m_freeSizeMb <= currentFreeSizeMb) {
                // nothing to do
                *needsVmOff = false;
                m_freeSizeMb = 0;
            }
            return Prepared;
        } else {
            *errorString = unknownPropertyMessage();
            return Ignored;
        }
    }

    bool set() override
    {
        bool ok = true;

        if (m_memorySizeMb != m_vm->memorySizeMb()) {
            bool stepOk;
            execAsynchronous(std::tie(stepOk), std::mem_fn(&VirtualMachine::setMemorySizeMb),
                    m_vm.data(), m_memorySizeMb);
            ok &= stepOk;
        }

        if (m_swapSizeMb != m_vm->swapSizeMb() && (m_vm->features() & VirtualMachine::SwapMemory)) {
            bool stepOk;
            execAsynchronous(std::tie(stepOk), std::mem_fn(&VirtualMachine::setSwapSizeMb),
                    m_vm.data(), m_swapSizeMb);
            ok &= stepOk;
        }

        if (m_cpuCount != m_vm->cpuCount()) {
            bool stepOk;
            execAsynchronous(std::tie(stepOk), std::mem_fn(&VirtualMachine::setCpuCount),
                    m_vm.data(), m_cpuCount);
            ok &= stepOk;
        }

        if (m_storageSizeMb != m_vm->storageSizeMb()) {
            bool stepOk;
            execAsynchronous(std::tie(stepOk), std::mem_fn(&VirtualMachine::setStorageSizeMb),
                    m_vm.data(), m_storageSizeMb);
            ok &= stepOk;
        }

        if (m_freeSizeMb > 0) {
            bool stepOk;
            execAsynchronous(std::tie(stepOk), std::mem_fn(&VirtualMachine::reserveStorageSizeMb),
                             m_vm.data(), m_freeSizeMb, m_incrementMb);
            return stepOk;
        }

        return ok;
    }

private:
    QPointer<VirtualMachine> m_vm;
    int m_memorySizeMb = 0;
    int m_swapSizeMb = 0;
    int m_cpuCount = 0;
    int m_storageSizeMb = 0;
    int m_freeSizeMb = 0;
    int m_incrementMb = 0;
};

/*!
 * \class SdkPropertiesAccessor
 */

class SdkPropertiesAccessor : public PropertiesAccessor
{
    Q_DECLARE_TR_FUNCTIONS(Sfdk::SdkPropertiesAccessor)

public:
    SdkPropertiesAccessor()
    {
        QTC_ASSERT(SdkManager::hasEngine(), return);
        m_wwwProxyType = SdkManager::engine()->wwwProxyType();
        m_wwwProxyServers = SdkManager::engine()->wwwProxyServers();
        m_wwwProxyExcludes = SdkManager::engine()->wwwProxyExcludes();
    }

    QMap<QString, QString> get() const override
    {
        QMap<QString, QString> values;
        values.insert(WWW_PROXY_TYPE, m_wwwProxyType);
        values.insert(WWW_PROXY_SERVERS, m_wwwProxyServers);
        values.insert(WWW_PROXY_EXCLUDES, m_wwwProxyExcludes);
        return values;
    }

    PrepareResult prepareSet(const QString &name, const QString &value, bool *needsVmOff,
            QString *errorString) override
    {
        *needsVmOff = false;

        auto validateUrls = [](const QString &urls, QString *errorString) {
            for (const QString &url : urls.split(' ', Qt::SkipEmptyParts)) {
                if (!QUrl::fromUserInput(url).isValid()) {
                    *errorString = tr("Not a valid URL: \"%1\"").arg(url);
                    return false;
                }
            }
            return true;
        };

        if (name == WWW_PROXY_TYPE) {
            if (value != Constants::WWW_PROXY_DISABLED
                    && value != Constants::WWW_PROXY_AUTOMATIC
                    && value != Constants::WWW_PROXY_MANUAL) {
                *errorString = tr("Invalid proxy type: \"%1\"").arg(value);
                return Failed;
            }
            m_wwwProxyType = value;
            return Prepared;
        } else if (name == WWW_PROXY_SERVERS) {
            if (!validateUrls(value, errorString))
                return Failed;
            m_wwwProxyServers = value.trimmed();
            return Prepared;
        } else if (name == WWW_PROXY_EXCLUDES) {
            if (!validateUrls(value, errorString))
                return Failed;
            m_wwwProxyExcludes = value.trimmed();
            return Prepared;
        } else {
            *errorString = unknownPropertyMessage();
            return Ignored;
        }
    }

    bool canSet(QString *errorString) const override
    {
        if ((m_wwwProxyType == Constants::WWW_PROXY_MANUAL
                    || m_wwwProxyType == Constants::WWW_PROXY_AUTOMATIC)
                && m_wwwProxyServers.isEmpty()) {
            *errorString = tr("The value of \"%1\" must not be empty when \"%2\" is set to \"%3\"")
                .arg(WWW_PROXY_SERVERS)
                .arg(WWW_PROXY_TYPE)
                .arg(m_wwwProxyType);
            return false;
        }

        return true;
    }

    bool set() override
    {
        QTC_ASSERT(SdkManager::hasEngine(), return false);
        BuildEngine *const engine = SdkManager::engine();

        if (m_wwwProxyType != engine->wwwProxyType()
                || m_wwwProxyServers != engine->wwwProxyServers()
                || m_wwwProxyExcludes != engine->wwwProxyExcludes()) {

            if (m_wwwProxyType == Constants::WWW_PROXY_AUTOMATIC
                    && m_wwwProxyServers.count() > 1) {
                qCWarning(sfdk).noquote()
                    << tr("Ignoring extra items in the \"%1\" list with \"%2\" set to \"%3\"")
                    .arg(WWW_PROXY_SERVERS)
                    .arg(WWW_PROXY_TYPE)
                    .arg(Constants::WWW_PROXY_AUTOMATIC);
            }

            engine->setWwwProxy(m_wwwProxyType, m_wwwProxyServers, m_wwwProxyExcludes);
        }

        return true;
    }

private:
    QString m_wwwProxyType;
    QString m_wwwProxyServers;
    QString m_wwwProxyExcludes;
};

/*!
 * \class EmulatorPropertiesAccessor
 */

class EmulatorPropertiesAccessor : public PropertiesAccessor
{
public:
    EmulatorPropertiesAccessor(Emulator *emulator)
        : m_emulator(emulator)
        , m_vmAccessor(std::make_unique<VirtualMachinePropertiesAccessor>(
                    emulator->virtualMachine()))
    {
        m_deviceModel = emulator->deviceModel().name;
        m_orientation = emulator->orientation();
        m_downscale = emulator->isViewScaled();
        m_sshPort = m_emulator->sshPort();
        m_sshTimeout = m_emulator->virtualMachine()->sshParameters().timeout;
    }

    QMap<QString, QString> get() const override
    {
        return m_vmAccessor->get().unite(getOthers());
    }

    PrepareResult prepareSet(const QString &name, const QString &value, bool *needsVmOff,
            QString *errorString) override
    {
        if (auto result = m_vmAccessor->prepareSet(name, value, needsVmOff, errorString))
            return result;
        if (auto result = prepareSetOther(name, value, needsVmOff, errorString))
            return result;
        return Ignored;
    }

    bool set() override
    {
        return m_vmAccessor->set() && setOthers();
    }

private:
    QMap<QString, QString> getOthers() const
    {
        QMap<QString, QString> values;
        values.insert(EMULATOR_DEVICE_MODEL, m_deviceModel);
        values.insert(EMULATOR_ORIENTATION, showOrientation(m_orientation));
        values.insert(EMULATOR_DOWNSCALE, showBoolean(m_downscale));

        // Access to emulators with custom SSH port set is currently broken with
        // the Docker-based build engine
        if (qEnvironmentVariableIntValue(Constants::I_KNOW_WHAT_I_AM_DOING_ENV_VAR)) {
            values.insert(EMULATOR_SSH_PORT, QString::number(m_sshPort));
            values.insert(EMULATOR_SSH_TIMEOUT, QString::number(m_sshTimeout));
        }

        return values;
    }

    PrepareResult prepareSetOther(const QString &name, const QString &value, bool *needsVmOff,
            QString *errorString)
    {
        *needsVmOff = true;

        if (name == EMULATOR_DEVICE_MODEL) {
            if (value.isEmpty()) {
                *errorString = valueEmptyMessage();
                return Failed;
            }
            if (!Sdk::deviceModel(value).isValid()) {
                *errorString = tr("No such device model: \"%1\"").arg(value);
                return Failed;
            }
            m_deviceModel = value;
            return Prepared;
        } else if (name == EMULATOR_ORIENTATION) {
            if (!parseOrientation(&m_orientation, value, errorString))
                return Failed;
            return Prepared;
        } else if (name == EMULATOR_DOWNSCALE) {
            if (!parseBoolean(&m_downscale, value, errorString))
                return Failed;
            return Prepared;
        } else if (name == EMULATOR_SSH_PORT) {
            if (!parsePortNumber(&m_sshPort, value, errorString))
                return Failed;
            *needsVmOff = true;
            return Prepared;
        } else if (name == EMULATOR_SSH_TIMEOUT) {
            if (!parsePositiveInt(&m_sshTimeout, value, errorString))
                return Failed;
            return Prepared;
        } else {
            *errorString = unknownPropertyMessage();
            return Ignored;
        }
    }

    bool setOthers()
    {
        bool ok = true;

        if (m_deviceModel != m_emulator->deviceModel().name
                || m_orientation != m_emulator->orientation()
                || m_downscale != m_emulator->isViewScaled()) {
            bool stepOk;
            execAsynchronous(std::tie(stepOk), std::mem_fn(&Emulator::setDisplayProperties),
                    m_emulator, Sdk::deviceModel(m_deviceModel), m_orientation, m_downscale);
            ok &= stepOk;
        }

        if (m_sshPort != m_emulator->sshPort()) {
            bool stepOk;
            execAsynchronous(std::tie(stepOk), std::mem_fn(&Emulator::setSshPort),
                    m_emulator.data(), m_sshPort);
            ok &= stepOk;
        }

        if (m_sshTimeout != m_emulator->virtualMachine()->sshParameters().timeout) {
            QSsh::SshConnectionParameters sshParameters =
                m_emulator->virtualMachine()->sshParameters();
            sshParameters.timeout = m_sshTimeout;
            m_emulator->virtualMachine()->setSshParameters(sshParameters);
        }

        return ok;
    }

private:
    QPointer<Emulator> m_emulator;
    std::unique_ptr<VirtualMachinePropertiesAccessor> m_vmAccessor;
    QString m_deviceModel;
    Qt::Orientation m_orientation{};
    bool m_downscale{};
    quint16 m_sshPort{};
    int m_sshTimeout{};
};

/*!
 * \class BuildEnginePropertiesAccessor
 */

class BuildEnginePropertiesAccessor : public PropertiesAccessor
{
public:
    BuildEnginePropertiesAccessor(BuildEngine *engine)
        : m_engine(engine)
        , m_vmAccessor(std::make_unique<VirtualMachinePropertiesAccessor>(
                    engine->virtualMachine()))
    {
        m_hostName = Sdk::effectiveBuildHostName();
        m_buildEnvironmentFilter = Sdk::buildEnvironmentFilter();
        m_sshPort = m_engine->sshPort();
        m_sshTimeout = m_engine->virtualMachine()->sshParameters().timeout;
        m_dBusPort = m_engine->dBusPort();
    }

    QMap<QString, QString> get() const override
    {
        return m_vmAccessor->get().unite(getOthers());
    }

    PrepareResult prepareSet(const QString &name, const QString &value, bool *needsVmOff,
            QString *errorString) override
    {
        if (auto result = m_vmAccessor->prepareSet(name, value, needsVmOff, errorString))
            return result;
        if (auto result = prepareSetOther(name, value, needsVmOff, errorString))
            return result;
        return Ignored;
    }

    bool set() override
    {
        return m_vmAccessor->set() && setOthers();
    }

private:
    QMap<QString, QString> getOthers() const
    {
        QMap<QString, QString> values;
        values.insert(ENGINE_HOST_NAME, m_hostName);
        values.insert(ENGINE_BUILD_ENVIRONMENT_FILTER, m_buildEnvironmentFilter.join(' '));
        values.insert(ENGINE_SSH_PORT, QString::number(m_sshPort));
        values.insert(ENGINE_SSH_TIMEOUT, QString::number(m_sshTimeout));
        values.insert(ENGINE_DBUS_PORT, QString::number(m_dBusPort));
        return values;
    }

    PrepareResult prepareSetOther(const QString &name, const QString &value, bool *needsVmOff,
            QString *errorString)
    {
        *needsVmOff = false;

        if (name == ENGINE_HOST_NAME) {
            if (!value.isEmpty()) {
                QUrl url;
                url.setHost(value);
                if (!url.isValid()) {
                    *errorString = tr("Not a well formed host name: \"%1\"").arg(value);
                    return Failed;
                }
            }
            m_hostName = value.isEmpty() ? QString() : value;
            m_hostNameChanged = true;
            return Prepared;
        } else if (name == ENGINE_BUILD_ENVIRONMENT_FILTER) {
            m_buildEnvironmentFilter = value.split(QRegularExpression("[[:space:]]+"),
                    Qt::SkipEmptyParts);
            return Prepared;
        } else if (name == ENGINE_DBUS_PORT) {
            if (!parsePortNumber(&m_dBusPort, value, errorString))
                return Failed;
            *needsVmOff = true;
            return Prepared;
        } else if (name == ENGINE_SSH_PORT) {
            if (!parsePortNumber(&m_sshPort, value, errorString))
                return Failed;
            *needsVmOff = true;
            return Prepared;
        } else if (name == ENGINE_SSH_TIMEOUT) {
            if (!parsePositiveInt(&m_sshTimeout, value, errorString))
                return Failed;
            return Prepared;
        } else {
            *errorString = unknownPropertyMessage();
            return Ignored;
        }
    }

    bool setOthers()
    {
        bool ok = true;

        if (m_hostNameChanged)
            Sdk::setCustomBuildHostName(m_hostName);

        Sdk::setBuildEnvironmentFilter(m_buildEnvironmentFilter);

        if (m_sshPort != m_engine->sshPort()) {
            bool stepOk;
            execAsynchronous(std::tie(stepOk), std::mem_fn(&BuildEngine::setSshPort),
                    m_engine.data(), m_sshPort);
            ok &= stepOk;
        }

        if (m_sshTimeout != m_engine->virtualMachine()->sshParameters().timeout) {
            QSsh::SshConnectionParameters sshParameters =
                m_engine->virtualMachine()->sshParameters();
            sshParameters.timeout = m_sshTimeout;
            m_engine->virtualMachine()->setSshParameters(sshParameters);
        }

        if (m_dBusPort != m_engine->dBusPort()) {
            bool stepOk;
            execAsynchronous(std::tie(stepOk), std::mem_fn(&BuildEngine::setDBusPort),
                    m_engine.data(), m_dBusPort);
            ok &= stepOk;
        }

        return ok;
    }

private:
    QPointer<BuildEngine> m_engine;
    std::unique_ptr<VirtualMachinePropertiesAccessor> m_vmAccessor;
    QString m_hostName;
    bool m_hostNameChanged = false;
    QStringList m_buildEnvironmentFilter;
    quint16 m_sshPort{};
    int m_sshTimeout{};
    quint16 m_dBusPort{};
};

} // namespace Sfdk

/*!
 * \class Command
 */

const Domain *Command::domain() const
{
    return module->domain;
}

/*!
 * \class Worker
 */

Worker::ExitStatus Worker::run(const Command *command, const QStringList &arguments_, int *exitCode)
    const
{
    auto doRunCommandLineFilter = [](const Command *command, QStringList *arguments,
            QString *errorString) {
        auto resultTypeValidator = [=](const QJSValue &value, QString *errorString) {
            if (!value.isArray() || !Utils::allOf(
                        Dispatcher::jsEngine()->fromScriptValue<QJSValueList>(value),
                        &QJSValue::isString)) {
                *errorString = "Not an array of strings";
                return false;
            }
            return true;
        };

        // FIXME?
        // QJSEngine::toScriptValue<QStringList>() seems to produce something else then JS Array.
        // When that value is later returned back to C++, QJSEngine::fromScriptValue<QStringList>()
        // fails on it.
        QJSValue argumentsArray = Dispatcher::jsEngine()->newArray(arguments->count());
        for (int i = 0; i < arguments->count(); ++i)
            argumentsArray.setProperty(i, arguments->at(i));

        const QJSValue result = Dispatcher::jsEngine()->call(command->commandLineFilterJSFunctionName,
                {argumentsArray}, command->module, resultTypeValidator);
        if (result.isError()) {
            *errorString = result.toString();
            return false;
        }

        qCDebug(sfdk) << "Original command line:" << *arguments;

        *arguments = Dispatcher::jsEngine()->fromScriptValue<QStringList>(result);

        qCDebug(sfdk) << "Filtered command line:" << *arguments;

        return true;
    };

    auto doRunPrePost = [](const Command *command, const QString &jsFunctionName,
            QString *errorString) {
        auto resultTypeValidator = [](const QJSValue &value, QString *errorString) {
            if (!value.isArray()
                    || value.property("length").toInt() < 2
                    || !value.property(0).isBool()
                    || !value.property(1).isString()) {
                *errorString = "Not an array [bool, string]";
                return false;
            }
            return true;
        };

        const QJSValue result = Dispatcher::jsEngine()->call(jsFunctionName, {}, command->module,
                resultTypeValidator);
        if (result.isError()) {
            *errorString = result.toString();
            return false;
        }

        *errorString = result.property(1).toString();
        return result.property(0).toBool();
    };

    QString errorString;

    QStringList arguments = arguments_;

    if (!command->commandLineFilterJSFunctionName.isEmpty()
            && !doRunCommandLineFilter(command, &arguments, &errorString)) {
        qerr() << tr("Command line filter routine failed: ") << errorString << endl;
        *exitCode = SFDK_EXIT_ABNORMAL;
        return NormalExit;
    }

    if (!command->preRunJSFunctionName.isEmpty()
            && !doRunPrePost(command, command->preRunJSFunctionName, &errorString)) {
        qerr() << tr("Pre-run routine failed: ") << errorString << endl;
        *exitCode = SFDK_EXIT_ABNORMAL;
        return NormalExit;
    }

    const ExitStatus status = doRun(command, arguments, exitCode);
    if (status != NormalExit)
        return status;

    if (!command->postRunJSFunctionName.isEmpty()
            && !doRunPrePost(command, command->postRunJSFunctionName, &errorString)) {
        qerr() << tr("Post-run routine failed: ") << errorString << endl;
        *exitCode = SFDK_EXIT_ABNORMAL;
        return NormalExit;
    }

    return NormalExit;
}

QString Worker::crashExitErrorMessage()
{
    return tr("Command exited abnormally");
}

bool Worker::checkVersion(int version, int minSupported, int maxSupported, QString *errorMessage)
{
    if (version < minSupported || version > maxSupported) {
        *errorMessage = tr("Version unsupported: %1").arg(version);
        return false;
    }
    return true;
}

/*!
 * \class BuiltinWorker
 */

Worker::ExitStatus BuiltinWorker::doRun(const Command *command, const QStringList &arguments,
        int *exitCode) const
{
    Q_ASSERT(exitCode != nullptr);
    *exitCode = EXIT_SUCCESS;

    QStringList arguments0 = arguments;
    arguments0.prepend(command->name);

    if (command->name == "config")
        return runConfig(arguments0);
    else if (command->name == "debug")
        return runDebug(arguments0, exitCode);
    else if (command->name == "device")
        return runDevice(arguments, exitCode);
    else if (command->name == "emulator")
        return runEmulator(arguments, exitCode);
    else if (command->name == "engine")
        return runEngine(arguments, exitCode);
    else if (command->name == "maintain")
        return runMaintain(arguments, exitCode);
    else if (command->name == "misc")
        return runMisc(arguments, exitCode);
    else if (command->name == "tools")
        return runTools(arguments, exitCode);

    qCCritical(sfdk) << "No such builtin:" << command->name << endl;
    return NoSuchCommand;
}

std::unique_ptr<Worker> BuiltinWorker::fromMap(const QVariantMap &data, int version,
        QString *errorString)
{
    if (!checkVersion(version, 1, 1, errorString))
        return {};

    if (!Dispatcher::checkKeys(data, {}, errorString))
        return {};

    auto worker = std::make_unique<BuiltinWorker>();

#ifdef Q_OS_MACOS
    return std::move(worker);
#else
    return worker;
#endif
}

Worker::ExitStatus BuiltinWorker::runConfig(const QStringList &arguments0) const
{
    using P = CommandLineParser;

    if (!Configuration::isLoaded()) {
        qerr() << P::commandNotAvailableMessage(arguments0.first()) << endl;
        return BadUsage;
    }

    QCommandLineParser parser;
    QCommandLineOption showOption("show");
    QCommandLineOption globalOption("global");
    QCommandLineOption sessionOption("session");
    QCommandLineOption pushOption("push");
    QCommandLineOption pushMaskOption("push-mask");
    QCommandLineOption dropOption("drop");
    parser.addOptions({showOption, globalOption, sessionOption, pushOption,
            pushMaskOption, dropOption});
    parser.addPositionalArgument("name", QString(), "[name]");
    parser.addPositionalArgument("value", QString(), "[value]");

    if (!parser.parse(arguments0)) {
        qerr() << parser.errorText() << endl;
        return BadUsage;
    }

    const QCommandLineOption *modeOption;
    if (!P::checkExclusiveOption(parser, {&showOption, &pushOption, &pushMaskOption, &dropOption},
                &modeOption)) {
        return BadUsage;
    }

    if (modeOption == nullptr && parser.positionalArguments().isEmpty())
        modeOption = &showOption;

    if (modeOption == &showOption) {
        if (!P::checkExclusiveOption(parser, {&showOption, &globalOption}))
            return BadUsage;
        if (!P::checkExclusiveOption(parser, {&showOption, &sessionOption}))
            return BadUsage;
        if (!parser.positionalArguments().isEmpty()) {
            qerr() << P::unexpectedArgumentMessage(parser.positionalArguments().first()) << endl;
            return BadUsage;
        }
        qout() << Configuration::print();
        return NormalExit;
    } else {
        Configuration::Scope scope = parser.isSet(globalOption)
            ? Configuration::Global
            : Configuration::Session;

        if (parser.positionalArguments().isEmpty()) {
            qerr() << P::missingArgumentMessage() << endl;
            return BadUsage;
        }

        if (modeOption == nullptr) {
            if (parser.positionalArguments().count() > 1) {
                qerr() << P::unexpectedArgumentMessage(parser.positionalArguments().at(1)) << endl;
                return BadUsage;
            }
            OptionOccurence occurence =
                OptionOccurence::fromString(parser.positionalArguments().first());
            if (occurence.isNull()) {
                qerr() << P::invalidPositionalArgumentMessage(occurence.errorString(),
                        parser.positionalArguments().first()) << endl;
                return BadUsage;
            }

            QString errorString;
            if (occurence.type() == OptionOccurence::Push && !occurence.argument().isEmpty()
                    && !occurence.isArgumentValid(&errorString)) {
                qerr() << P::invalidPositionalArgumentMessage(errorString, occurence.argument())
                    << endl;
                return BadUsage;
            }

            Configuration::push(scope, occurence);
            return NormalExit;
        }

        const QString name = parser.positionalArguments().first();
        const Option *const option = Dispatcher::option(name);
        if (option == nullptr) {
            qerr() << P::unrecognizedOptionMessage(name) << endl;
            return BadUsage;
        }
        if ((modeOption != &pushOption || option->argumentType == Option::NoArgument)
                && parser.positionalArguments().count() > 1) {
            qerr() << P::unexpectedArgumentMessage(parser.positionalArguments().at(1)) << endl;
            return BadUsage;
        }
        if (modeOption == &pushOption && option->argumentType == Option::MandatoryArgument
                && parser.positionalArguments().count() != 2) {
            if (parser.positionalArguments().count() < 2)
                qerr() << P::missingArgumentMessage() << endl;
            else
                qerr() << P::unexpectedArgumentMessage(parser.positionalArguments().at(2)) << endl;
            return BadUsage;
        }

        if (modeOption == &pushOption) {
            const QString argument = parser.positionalArguments().count() == 2
                ? parser.positionalArguments().last()
                : QString();
            const OptionOccurence occurence(option, OptionOccurence::Push, argument);

            QString errorString;
            if (!argument.isEmpty() && !occurence.isArgumentValid(&errorString)) {
                qerr() << P::invalidPositionalArgumentMessage(errorString, argument) << endl;
                return BadUsage;
            }

            Configuration::push(scope, occurence);
        } else if (modeOption == &pushMaskOption) {
            Configuration::pushMask(scope, option);
        } else if (modeOption == &dropOption) {
            Configuration::drop(scope, option);
        } else {
            Q_ASSERT(false);
        }

        return NormalExit;
    }
}

Worker::ExitStatus BuiltinWorker::runDebug(const QStringList &arguments0, int *exitCode) const
{
    using P = CommandLineParser;

    enum Subcommand {
        Invalid,
        Start,
        Attach,
        LoadCore,
    };

    QCommandLineParser parser;
    parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);

    // Options related to GDB invocation
    QCommandLineOption dryRunOption(QStringList{"dry-run", "n"});
    QCommandLineOption gdbOption("gdb", QString(), "<executable>");
    QCommandLineOption gdbArgsOption("gdb-args", QString(), "<args>");
    QCommandLineOption gdbServerOption("gdbserver", QString(), "<executable>");
    QCommandLineOption gdbServerArgsOption("gdbserver-args", QString(), "<args>");
    QList<QCommandLineOption> gdbInvocationOptions{dryRunOption, gdbOption, gdbArgsOption,
            gdbServerOption, gdbServerArgsOption};

    // Options specific to the "start" subcommand
    QCommandLineOption workingDirectoryOption(QStringList{"working-directory", "C"}, QString(),
            "<path>");
    QCommandLineOption argsOption("args");
    QList<QCommandLineOption> startOptions{workingDirectoryOption, argsOption};

    // Options specific to the "load-core" subcommand
    QCommandLineOption localCoreOption("local-core");
    QList<QCommandLineOption> loadCoreOptions{localCoreOption};

    if (arguments0.count() < 2) {
        qerr() << P::missingArgumentMessage() << endl;
        return BadUsage;
    }

    Subcommand subcommand = Invalid;

    const QString maybeSubcommand = arguments0.at(1);
    if (maybeSubcommand == "start") {
        subcommand = Start;
        parser.addOptions(gdbInvocationOptions);
        parser.addOptions(startOptions);
        if (!parser.parse(arguments0.mid(1))) {
            qerr() << parser.errorText() << endl;
            return BadUsage;
        }
        const int maxArgs = parser.isSet(argsOption) ? -1 : 2;
        if (!P::checkPositionalArgumentsCount(parser.positionalArguments(), 1, maxArgs))
            return BadUsage;
    } else if (maybeSubcommand == "attach") {
        subcommand = Attach;
        parser.addOptions(gdbInvocationOptions);
        if (!parser.parse(arguments0.mid(1))) {
            qerr() << parser.errorText() << endl;
            return BadUsage;
        }
        if (!P::checkPositionalArgumentsCount(parser.positionalArguments(), 2, 2))
            return BadUsage;
        if (parser.positionalArguments().at(1).toInt() == 0) {
            qerr() << tr("Not a valid process ID: '%1'").arg(parser.positionalArguments().at(1))
                << endl;
            return BadUsage;
        }
    } else if (maybeSubcommand == "load-core") {
        subcommand = LoadCore;
        parser.addOptions(gdbInvocationOptions);
        parser.addOptions(loadCoreOptions);
        if (!parser.parse(arguments0.mid(1))) {
            qerr() << parser.errorText() << endl;
            return BadUsage;
        }
        if (!P::checkPositionalArgumentsCount(parser.positionalArguments(), 2, 2))
            return BadUsage;
    } else {
        // Subcommand was not specified explicitly, guess it
        parser.addOptions(gdbInvocationOptions);
        parser.addOptions(startOptions);
        parser.addOptions(loadCoreOptions);
        if (!parser.parse(arguments0)) {
            qerr() << parser.errorText() << endl;
            return BadUsage;
        }

        if (parser.positionalArguments().isEmpty()) {
            qerr() << P::missingArgumentMessage() << endl;
            return BadUsage;
        } else if (parser.positionalArguments().count() == 1 || parser.isSet(argsOption)) {
            subcommand = Start;
        } else if (parser.positionalArguments().count() > 2) {
            qerr() << P::unexpectedArgumentMessage(parser.positionalArguments().at(2)) << endl;
            return BadUsage;
        } else if (parser.positionalArguments().at(1).toInt() > 0) {
            subcommand = Attach;
        } else {
            subcommand = LoadCore;
        }
        QTC_ASSERT(subcommand != Invalid, return NormalExit);

        if (parser.isSet(workingDirectoryOption) && subcommand != Start) {
            qerr() << P::optionNotAvailableMessage(workingDirectoryOption.names().first()) << endl;
            return BadUsage;
        }
        if (parser.isSet(argsOption) && subcommand != Start) {
            qerr() << P::optionNotAvailableMessage(argsOption.names().first()) << endl;
            return BadUsage;
        }
        if (parser.isSet(localCoreOption) && subcommand != LoadCore) {
            qerr() << P::optionNotAvailableMessage(localCoreOption.names().first()) << endl;
            return BadUsage;
        }
    }
    QTC_ASSERT(subcommand != Invalid, return NormalExit);

    QString errorString;

    Device *const device = SdkManager::configuredDevice(&errorString);
    if (!device) {
        qerr() << errorString << endl;
        *exitCode = SFDK_EXIT_ABNORMAL;
        return NormalExit;
    }

    const BuildTargetData target = SdkManager::configuredTarget(&errorString);
    if (!target.isValid()) {
        qerr() << errorString << endl;
        *exitCode = SFDK_EXIT_ABNORMAL;
        return NormalExit;
    }

    Debugger debugger(device, target);

    debugger.setDryRunEnabled(parser.isSet(dryRunOption));
    if (parser.isSet(gdbOption))
        debugger.setGdbExecutable(parser.value(gdbOption));
    if (parser.isSet(gdbArgsOption)) {
        QStringList split;
        if (!CommandLineParser::splitArgs(parser.value(gdbArgsOption), Utils::OsTypeLinux, &split))
            return BadUsage;
        debugger.setGdbExtraArgs(split);
    }
    if (parser.isSet(gdbServerOption))
        debugger.setGdbServerExecutable(parser.value(gdbServerOption));
    if (parser.isSet(gdbServerArgsOption)) {
        QStringList split;
        if (!CommandLineParser::splitArgs(parser.value(gdbServerArgsOption), Utils::OsTypeLinux, &split))
            return BadUsage;
        debugger.setGdbServerExtraArgs(split);
    }

    switch (subcommand) {
    case Invalid:
        QTC_ASSERT(false, return NormalExit);
    case Start:
        *exitCode = debugger.execStart(parser.positionalArguments().first(),
                parser.positionalArguments().mid(1),
                parser.value(workingDirectoryOption));
        return NormalExit;
    case Attach:
        *exitCode = debugger.execAttach(parser.positionalArguments().first(),
                parser.positionalArguments().at(1).toInt());
        return NormalExit;
    case LoadCore:
        *exitCode = debugger.execLoadCore(parser.positionalArguments().first(),
                parser.positionalArguments().at(1),
                parser.isSet(localCoreOption));
        return NormalExit;
    }

    *exitCode = EXIT_SUCCESS;
    return NormalExit;
}

Worker::ExitStatus BuiltinWorker::runDevice(const QStringList &arguments_, int *exitCode) const
{
    using P = CommandLineParser;

    QStringList arguments = arguments_;

    if (arguments.count() < 1) {
        qerr() << P::missingArgumentMessage() << endl;
        return BadUsage;
    }

    if (arguments.first() == "list") {
        if (arguments.count() > 1) {
            qerr() << P::unexpectedArgumentMessage(arguments.at(1)) << endl;
            return BadUsage;
        }
        listDevices();
        *exitCode = EXIT_SUCCESS;
        return NormalExit;
    }

    QString errorString;
    Device *device;
    // Device name may not start with '-'
    if (arguments.count() < 2 || arguments.at(1).startsWith('-')) {
        device = SdkManager::configuredDevice(&errorString);
    } else {
        device = deviceForNameOrIndex(arguments.at(1), &errorString);
        if (device) {
            arguments.removeAt(1);
        } else if (arguments.first() == "exec") {
            // When "--" is used, then the very first argument must be either
            // an option (which would be caught by the top-level if/else) or
            // it must be a device name.
            if (!arguments.contains("--"))
                device = SdkManager::configuredDevice(&errorString);
        }
    }
    if (!device) {
        qerr() << errorString << endl;
        *exitCode = SFDK_EXIT_ABNORMAL;
        return NormalExit;
    }

    if (arguments.first() == "exec") {
        QCommandLineParser parser;
        QCommandLineOption tOption("t");

        parser.addOptions({tOption});
        parser.addPositionalArgument("command", QString(), "[command]");
        parser.addPositionalArgument("args", QString(), "[args...]");
        parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);

        if (!parser.parse(arguments)) {
            qerr() << parser.errorText() << endl;
            return BadUsage;
        }

        const QStringList command = parser.positionalArguments();

        QString program;
        QStringList programArguments;
        Utils::optional<bool> runInTerminal;
        if (!command.isEmpty()) {
            program = command.first();
            if (command.count() > 1)
                programArguments = command.mid(1);
            if (!parser.isSet(tOption))
                runInTerminal = false;
            else if (CommandLineParser::optionCount(parser, tOption) > 1)
                runInTerminal = true;
        } else {
            program = "/bin/bash";
            programArguments << "--login";
            if (CommandLineParser::optionCount(parser, tOption) > 1)
                runInTerminal = true;
        }

        *exitCode = SdkManager::runOnDevice(*device, program, programArguments, runInTerminal);
        return NormalExit;
    }

    qerr() << P::unrecognizedCommandMessage(arguments.first()) << endl;
    return BadUsage;
}

Worker::ExitStatus BuiltinWorker::runEmulator(const QStringList &arguments_, int *exitCode) const
{
    using P = CommandLineParser;

    QStringList arguments = arguments_;

    if (arguments.count() < 1) {
        qerr() << P::missingArgumentMessage() << endl;
        return BadUsage;
    }

    if (arguments.first() == "list") {
        QCommandLineParser parser;
        QCommandLineOption availableOption(QStringList{"available", "a"});
        parser.addOptions({availableOption});

        if (!parser.parse(arguments)) {
            qerr() << parser.errorText() << endl;
            return BadUsage;
        }

        SdkManager::ListEmulatorsOptions options = SdkManager::InstalledEmulators;
        if (parser.isSet(availableOption))
            options = options | SdkManager::AvailableEmulators;
        else
            options = options | SdkManager::UserDefinedEmulators;

        *exitCode = listEmulators(options) ? EXIT_SUCCESS : EXIT_FAILURE;

        return NormalExit;
    }

    if (arguments.first() == "install") {
        if (!P::checkPositionalArgumentsCount(arguments, 2, 2))
            return BadUsage;

        const QString name = arguments.at(1);

        *exitCode = SdkManager::installEmulator(name) ? EXIT_SUCCESS : EXIT_FAILURE;
        return NormalExit;
    }

    if (arguments.first() == "remove") {
        if (!P::checkPositionalArgumentsCount(arguments, 2, 2))
            return BadUsage;

        const QString name = arguments.at(1);

        *exitCode = SdkManager::removeEmulator(name) ? EXIT_SUCCESS : EXIT_FAILURE;
        return NormalExit;
    }

    if (arguments.first() == "device-model-list") {
        if (!P::checkPositionalArgumentsCount(arguments, 1, 1))
            return BadUsage;
        listDeviceModels();
        *exitCode = EXIT_SUCCESS;
        return NormalExit;
    }

    if (arguments.first() == "device-model-show") {
        if (!P::checkPositionalArgumentsCount(arguments, 2, 2))
            return BadUsage;

        const QString name = arguments.at(1);
        *exitCode = showDeviceModel(name) ? EXIT_SUCCESS : EXIT_FAILURE;
        return NormalExit;
    }

    QString errorString;
    Emulator *emulator;
    // Emulator name may not start with '-'
    if (arguments.count() < 2 || arguments.at(1).startsWith('-')) {
        emulator = defaultEmulator(&errorString);
    } else {
        emulator = SdkManager::emulatorByName(arguments.at(1), &errorString);
        if (emulator) {
            arguments.removeAt(1);
        } else if (arguments.first() == "exec" || arguments.first() == "set") {
            // When "--" is used, then the very first argument must be either
            // an option (which would be caught by the top-level if/else) or
            // it must be an emulator name.
            if (!arguments.contains("--"))
                emulator = defaultEmulator(&errorString);
        }
    }
    if (!emulator) {
        qerr() << errorString << endl;
        *exitCode = SFDK_EXIT_ABNORMAL;
        return NormalExit;
    }

    if (arguments.first() == "start") {
        if (arguments.count() > 2) {
            qerr() << P::unexpectedArgumentMessage(arguments.at(2)) << endl;
            return BadUsage;
        }
        *exitCode = SdkManager::startEmulator(*emulator) ? EXIT_SUCCESS : EXIT_FAILURE;
        return NormalExit;
    }

    if (arguments.first() == "stop") {
        if (arguments.count() > 2) {
            qerr() << P::unexpectedArgumentMessage(arguments.at(2)) << endl;
            return BadUsage;
        }
        *exitCode = SdkManager::stopEmulator(*emulator) ? EXIT_SUCCESS : EXIT_FAILURE;
        return NormalExit;
    }

    if (arguments.first() == "status") {
        if (arguments.count() > 2) {
            qerr() << P::unexpectedArgumentMessage(arguments.at(2)) << endl;
            return BadUsage;
        }
        bool running = SdkManager::isEmulatorRunning(*emulator);
        qout() << runningYesNoMessage(running) << endl;
        *exitCode = EXIT_SUCCESS;
        return NormalExit;
    }

    if (arguments.first() == "show") {
        if (arguments.count() > 2) {
            qerr() << P::unexpectedArgumentMessage(arguments.at(2)) << endl;
            return BadUsage;
        }
        printProperties(EmulatorPropertiesAccessor(emulator));
        *exitCode = EXIT_SUCCESS;
        return NormalExit;
    }

    if (arguments.first() == "set") {
        QStringList assignments = arguments.mid(1);
        if (!assignments.isEmpty() && assignments.first() == "--")
            assignments.removeFirst();

        if (assignments.isEmpty()) {
            qerr() << P::missingArgumentMessage() << endl;
            return BadUsage;
        }

        SetPropertiesTask task(
                std::make_unique<EmulatorPropertiesAccessor>(emulator),
                emulator->virtualMachine(),
                tr("Some of the changes cannot be applied while the emulator is running."
                    " Please stop the emulator."));
        return setProperties(&task, assignments, exitCode);
    }

    if (arguments.first() == "exec") {
        QCommandLineParser parser;
        QCommandLineOption tOption("t");

        parser.addOptions({tOption});
        parser.addPositionalArgument("command", QString(), "[command]");
        parser.addPositionalArgument("args", QString(), "[args...]");
        parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);

        if (!parser.parse(arguments)) {
            qerr() << parser.errorText() << endl;
            return BadUsage;
        }

        const QStringList command = parser.positionalArguments();

        QString program;
        QStringList programArguments;
        Utils::optional<bool> runInTerminal;
        if (!command.isEmpty()) {
            program = command.first();
            if (command.count() > 1)
                programArguments = command.mid(1);
            if (!parser.isSet(tOption))
                runInTerminal = false;
            else if (CommandLineParser::optionCount(parser, tOption) > 1)
                runInTerminal = true;
        } else {
            program = "/bin/bash";
            programArguments << "--login";
            if (CommandLineParser::optionCount(parser, tOption) > 1)
                runInTerminal = true;
        }

        *exitCode = SdkManager::runOnEmulator(*emulator, program, programArguments, runInTerminal);
        return NormalExit;
    }

    qerr() << P::unrecognizedCommandMessage(arguments.first()) << endl;
    return BadUsage;
}

Worker::ExitStatus BuiltinWorker::runEngine(const QStringList &arguments, int *exitCode) const
{
    using P = CommandLineParser;

    if (!SdkManager::hasEngine()) {
        qerr() << SdkManager::noEngineFoundMessage() << endl;
        *exitCode = SFDK_EXIT_ABNORMAL;
        return NormalExit;
    }

    if (arguments.count() < 1) {
        qerr() << P::missingArgumentMessage() << endl;
        return BadUsage;
    }

    if (arguments.first() == "start") {
        if (arguments.count() > 1) {
            qerr() << P::unexpectedArgumentMessage(arguments.at(1)) << endl;
            return BadUsage;
        }
        *exitCode = SdkManager::startEngine() ? EXIT_SUCCESS : EXIT_FAILURE;
        return NormalExit;
    }

    if (arguments.first() == "stop") {
        if (arguments.count() > 1) {
            qerr() << P::unexpectedArgumentMessage(arguments.at(1)) << endl;
            return BadUsage;
        }
        *exitCode = SdkManager::stopEngine() ? EXIT_SUCCESS : EXIT_FAILURE;
        return NormalExit;
    }

    if (arguments.first() == "status") {
        if (arguments.count() > 1) {
            qerr() << P::unexpectedArgumentMessage(arguments.at(1)) << endl;
            return BadUsage;
        }
        bool running = SdkManager::isEngineRunning();
        qout() << runningYesNoMessage(running) << endl;
        *exitCode = EXIT_SUCCESS;
        return NormalExit;
    }

    if (arguments.first() == "show") {
        if (arguments.count() > 1) {
            qerr() << P::unexpectedArgumentMessage(arguments.at(1)) << endl;
            return BadUsage;
        }
        printProperties(BuildEnginePropertiesAccessor(SdkManager::engine()));
        *exitCode = EXIT_SUCCESS;
        return NormalExit;
    }

    if (arguments.first() == "set") {
        const QStringList assignments = arguments.mid(1);

        if (assignments.isEmpty()) {
            qerr() << P::missingArgumentMessage() << endl;
            return BadUsage;
        }

        SetPropertiesTask task(
                std::make_unique<BuildEnginePropertiesAccessor>(SdkManager::engine()),
                SdkManager::engine()->virtualMachine(),
                tr("Some of the changes cannot be applied while the build engine is running."
                    " Please stop the build engine."));
        return setProperties(&task, assignments, exitCode);
    }

    if (arguments.first() == "exec") {
        QCommandLineParser parser;
        QCommandLineOption tOption("t");

        parser.addOptions({tOption});
        parser.addPositionalArgument("command", QString(), "[command]");
        parser.addPositionalArgument("args", QString(), "[args...]");
        parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);

        if (!parser.parse(arguments)) {
            qerr() << parser.errorText() << endl;
            return BadUsage;
        }

        const QStringList command = parser.positionalArguments();

        QString program;
        QStringList programArguments;
        Utils::optional<bool> runInTerminal;
        if (!command.isEmpty()) {
            program = command.at(0);
            if (command.count() > 1)
                programArguments = command.mid(1);
            if (!parser.isSet(tOption))
                runInTerminal = false;
            else if (CommandLineParser::optionCount(parser, tOption) > 1)
                runInTerminal = true;
        } else {
            SdkManager::setEnableReversePathMapping(false);
            program = "/bin/bash";
            programArguments << "--login";
            if (CommandLineParser::optionCount(parser, tOption) > 1)
                runInTerminal = true;
        }

        *exitCode = SdkManager::runOnEngine(program, programArguments, {}, runInTerminal);
        return NormalExit;
    }

    qerr() << P::unrecognizedCommandMessage(arguments.first()) << endl;
    return BadUsage;
}

Worker::ExitStatus BuiltinWorker::runMaintain(const QStringList &arguments, int *exitCode) const
{
    using P = CommandLineParser;

    if (!arguments.isEmpty()) {
        qerr() << P::unexpectedArgumentMessage(arguments.first()) << endl;
        return BadUsage;
    }

    *exitCode = QProcess::startDetached(SdkManager::sdkMaintenanceToolPath(), {})
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
    return NormalExit;
}

Worker::ExitStatus BuiltinWorker::runMisc(const QStringList &arguments, int *exitCode) const
{
    using P = CommandLineParser;

    if (arguments.count() < 1) {
        qerr() << P::missingArgumentMessage() << endl;
        return BadUsage;
    }

    if (arguments.first() == "stop-vms") {
        if (arguments.count() > 1) {
            qerr() << P::unexpectedArgumentMessage(arguments.at(1)) << endl;
            return BadUsage;
        }
        *exitCode = stopVirtualMachines() ? EXIT_SUCCESS : EXIT_FAILURE;
        return NormalExit;
    }

    if (arguments.first() == "show") {
        if (!SdkManager::hasEngine()) {
            qerr() << SdkManager::noEngineFoundMessage() << endl;
            *exitCode = SFDK_EXIT_ABNORMAL;
            return NormalExit;
        }

        if (arguments.count() > 1) {
            qerr() << P::unexpectedArgumentMessage(arguments.at(1)) << endl;
            return BadUsage;
        }
        printProperties(SdkPropertiesAccessor());
        *exitCode = EXIT_SUCCESS;
        return NormalExit;
    }

    if (arguments.first() == "set") {
        if (!SdkManager::hasEngine()) {
            qerr() << SdkManager::noEngineFoundMessage() << endl;
            *exitCode = SFDK_EXIT_ABNORMAL;
            return NormalExit;
        }

        const QStringList assignments = arguments.mid(1);

        if (assignments.isEmpty()) {
            qerr() << P::missingArgumentMessage() << endl;
            return BadUsage;
        }

        SetPropertiesTask task(
                std::make_unique<SdkPropertiesAccessor>(),
                SdkManager::engine()->virtualMachine(),
                tr("Some of the changes cannot be applied while the build engine is running."
                    " Please stop the build engine."));
        return setProperties(&task, assignments, exitCode);
    }

    // Internal command, intentionally undocumented
    if (arguments.first() == "inspect") {
        using P = CommandLineParser;

        if (!P::checkPositionalArgumentsCount(arguments, 2, 2))
            return BadUsage;

        const QString query = arguments.at(1);
        *exitCode = EXIT_FAILURE;

        if (query == "targets") {
            QTC_ASSERT(SdkManager::hasEngine(), return NormalExit);
            const Option *const noSnapshotOption_ = Dispatcher::option(Constants::NO_SNAPSHOT_OPTION_NAME);
            QTC_ASSERT(noSnapshotOption_, return {});
            const Utils::optional<OptionEffectiveOccurence> noSnapshotOption =
                Configuration::effectiveState(noSnapshotOption_);
            if (noSnapshotOption)
                qout() << SdkManager::engine()->buildTargetNames().join('\n') << endl;
            else
                qout() << SdkManager::engine()->buildTargetOrigins().join('\n') << endl;
        } else if (query == "commands") {
            for (const std::unique_ptr<const Command> &command : Dispatcher::commands())
                qout() << command->name << endl;
        } else if (query == "devices") {
            for (Device *const device : Sdk::devices())
                qout() << device->name() << endl;
        } else if (query == "domains") {
            for (const std::unique_ptr<const Domain> &domain : Dispatcher::domains())
                qout() << domain->name << endl;
        } else if (query == "emulators") {
            for (Emulator *const emulator : Sdk::emulators())
                qout() << emulator->name() << endl;
        } else if (query == "options") {
            for (const std::unique_ptr<const Option> &option : Dispatcher::options())
                qout() << option->name << '=' << option->argumentDescription << endl;
        } else if (query == "option-aliases") {
            for (const std::unique_ptr<const Option> &option : Dispatcher::options()) {
                if (!option->alias.isNull())
                    qout() << option->alias << '=' << option->name << endl;
            }
        } else if (query == "workspace") {
            QTC_ASSERT(SdkManager::hasEngine(), return NormalExit);
            qout() << SdkManager::engine()->sharedSrcPath().toString() << endl;
        } else {
            qerr() << tr("Not a valid inspection query: \"%1\"").arg(query) << endl;
            return NormalExit;
        }

        *exitCode = EXIT_SUCCESS;
        return NormalExit;
    }

    qerr() << P::unrecognizedCommandMessage(arguments.first()) << endl;
    return BadUsage;
}

Worker::ExitStatus BuiltinWorker::runTools(const QStringList &arguments_, int *exitCode) const
{
    using P = CommandLineParser;

    if (!SdkManager::hasEngine()) {
        qerr() << SdkManager::noEngineFoundMessage() << endl;
        *exitCode = SFDK_EXIT_ABNORMAL;
        return NormalExit;
    }

    // Process the optional tooling|target keyword first...
    QStringList arguments = arguments_;

    if (arguments.count() < 1) {
        qerr() << P::missingArgumentMessage() << endl;
        return BadUsage;
    }

    const SdkManager::ToolsTypeHint typeHint =
        arguments.first() == "tooling"
        ? SdkManager::ToolingHint
        : arguments.first() == "target"
            ? SdkManager::TargetHint
            : SdkManager::NoToolsHint;
    if (typeHint != SdkManager::NoToolsHint)
        arguments.removeFirst();

    // ...then the atual command
    if (arguments.count() < 1) {
        qerr() << P::missingArgumentMessage() << endl;
        return BadUsage;
    }

    if (arguments.first() == "list") {
        QCommandLineParser parser;
        QCommandLineOption availableOption(QStringList{"available", "a"});
        QCommandLineOption snapshotsOption(QStringList{"snapshots", "s"});
        QCommandLineOption slowOption("slow");
        parser.addOptions({availableOption, snapshotsOption, slowOption});

        if (!parser.parse(arguments)) {
            qerr() << parser.errorText() << endl;
            return BadUsage;
        }

        if (!P::checkExclusiveOption(parser, {&availableOption, &snapshotsOption}))
            return BadUsage;

        if (parser.isSet(snapshotsOption) && typeHint == SdkManager::ToolingHint) {
            qerr() << tr("The '%1' option cannot be used with toolings")
                .arg(snapshotsOption.names().first()) << endl;
            return BadUsage;
        }

        SdkManager::ListToolsOptions options = SdkManager::InstalledTools;
        if (parser.isSet(availableOption))
            options = options | SdkManager::AvailableTools;
        else
            options = options | SdkManager::UserDefinedTools;

        if (parser.isSet(snapshotsOption)) {
            options = options | SdkManager::SnapshotTools;
            if (parser.isSet(slowOption))
                options = options | SdkManager::CheckSnapshots;
        }

        const bool listToolings = typeHint == SdkManager::NoToolsHint || typeHint == SdkManager::ToolingHint;
        const bool listTargets = typeHint == SdkManager::NoToolsHint || typeHint == SdkManager::TargetHint;

        *exitCode = listTools(options, listToolings, listTargets) ? EXIT_SUCCESS : EXIT_FAILURE;
        return NormalExit;
    }

    if (arguments.first() == "update") {
        if (!P::checkPositionalArgumentsCount(arguments, 2, 2))
            return BadUsage;

        const QString name = arguments.at(1);

        *exitCode = SdkManager::updateTools(name, typeHint) ? EXIT_SUCCESS : EXIT_FAILURE;
        return NormalExit;
    }

    if (arguments.first() == "register") {
        QCommandLineParser parser;
        QCommandLineOption allOption("all");
        QCommandLineOption userOption("user", QString(), "name");
        QCommandLineOption passwordOption("password", QString(), "password");

        parser.addOptions({allOption, userOption, passwordOption});
        parser.addPositionalArgument("name", QString(), "[name]");

        if (!parser.parse(arguments)) {
            qerr() << parser.errorText() << endl;
            return BadUsage;
        }
        if (!(parser.isSet(allOption) == parser.positionalArguments().isEmpty())) {
            qerr() << tr("Exactly one of '%1' or '%2' expected")
                .arg(allOption.names().first())
                .arg("name") << endl;
            return BadUsage;
        }
        if (parser.positionalArguments().count() > 1) {
            qerr() << P::unexpectedArgumentMessage(parser.positionalArguments().at(1)) << endl;
            return BadUsage;
        }

        const QString maybeName = parser.isSet(allOption)
            ? QString()
            : parser.positionalArguments().first();
        const QString maybeUserName = parser.value(userOption);
        const QString maybePassword = parser.value(passwordOption);

        *exitCode = SdkManager::registerTools(maybeName, typeHint, maybeUserName, maybePassword)
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
        return NormalExit;
    }

    if (arguments.first() == "install") {
        if (!P::checkPositionalArgumentsCount(arguments, 2, 2))
            return BadUsage;

        const QString name = arguments.at(1);

        *exitCode = SdkManager::installTools(name, typeHint) ? EXIT_SUCCESS : EXIT_FAILURE;
        return NormalExit;
    }

    if (arguments.first() == "create") {
        arguments.replace(0, "install-custom");
        qerr() << P::commandDeprecatedMessage("create", "install-custom") << endl;
    }

    if (arguments.first() == "install-custom") {
        QCommandLineParser parser;
        QCommandLineOption toolingOption("tooling", QString(), "tooling");
        QCommandLineOption noSnapshotOption("no-snapshot");

        parser.addOptions({toolingOption, noSnapshotOption});
        parser.addPositionalArgument("name", QString(), "[name]");
        parser.addPositionalArgument("URL|file", QString(), "[URL|file]");

        if (!parser.parse(arguments)) {
            qerr() << parser.errorText() << endl;
            return BadUsage;
        }

        if (!P::checkPositionalArgumentsCount(parser.positionalArguments(), 2, 2))
            return BadUsage;

        const QString name = parser.positionalArguments().at(0);
        const QString imageFileOrUrl = parser.positionalArguments().at(1);
        const QString maybeTooling = parser.value(toolingOption);
        const bool noSnapshot = parser.isSet(noSnapshotOption);

        *exitCode = SdkManager::installCustomTools(name, imageFileOrUrl, typeHint, maybeTooling,
                noSnapshot)
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
        return NormalExit;
    }

    if (arguments.first() == "clone") {
        QCommandLineParser parser;

        parser.addPositionalArgument("name", QString());
        parser.addPositionalArgument("clone-name", QString());

        if (!parser.parse(arguments)) {
            qerr() << parser.errorText() << endl;
            return BadUsage;
        }

        if (!P::checkPositionalArgumentsCount(parser.positionalArguments(), 2, 2))
            return BadUsage;

        const QString name = parser.positionalArguments().at(0);
        const QString cloneName = parser.positionalArguments().at(1);

        *exitCode = SdkManager::cloneTools(name, cloneName, typeHint)
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
        return NormalExit;
    }

    if (arguments.first() == "remove") {
        QCommandLineParser parser;
        QCommandLineOption snapshotsOfOption("snapshots-of");

        parser.addOptions({snapshotsOfOption});
        parser.addPositionalArgument("name", QString(), "[name]");

        if (!parser.parse(arguments)) {
             qerr() << parser.errorText() << endl;
             return BadUsage;
        }

        if (!P::checkPositionalArgumentsCount(parser.positionalArguments(), 1, 1))
            return BadUsage;

        const QString name = parser.positionalArguments().first();

        *exitCode = SdkManager::removeTools(name, typeHint, parser.isSet(snapshotsOfOption))
            ? EXIT_SUCCESS : EXIT_FAILURE;
        return NormalExit;
    }

    if (arguments.first() == "package-list"
            || arguments.first() == "package-search"
            || arguments.first() == "package-install"
            || arguments.first() == "package-remove") {
        QStringList allArguments = arguments_;
        allArguments.prepend("--non-interactive");
        *exitCode = SdkManager::runOnEngine("sdk-assistant", allArguments);
        return NormalExit;
    }

    if (arguments.first() == "exec") {
        QStringList allArguments = arguments_;
        // sdk-assistant uses different name for this command
        allArguments[typeHint != SdkManager::NoToolsHint ? 1 : 0] = "maintain";
        allArguments.prepend("--non-interactive");
        if (arguments.count() <= 2)
            SdkManager::setEnableReversePathMapping(false);
        *exitCode = SdkManager::runOnEngine("sdk-assistant", allArguments);
        return NormalExit;
    }

    qerr() << P::unrecognizedCommandMessage(arguments.first()) << endl;
    return BadUsage;
}

void BuiltinWorker::listDevices()
{
    auto maxLength = [](const QStringList &strings) {
        const QList<int> lengths = Utils::transform(strings, &QString::length);
        return *std::max_element(lengths.begin(), lengths.end());
    };

    const QString hardwareType = tr("hardware-device");
    const QString emulatorType = tr("emulator");
    const int typeFieldWidth = maxLength({hardwareType, emulatorType});

    const QString autodetected = SdkManager::stateAutodetectedMessage();
    const QString userDefined = SdkManager::stateUserDefinedMessage();
    const int autodetectedFieldWidth = maxLength({autodetected, userDefined});

    int index = 0;
    for (Device *const device : Sdk::devices()) {
        const QString type = device->machineType() == Device::HardwareMachine
            ? hardwareType
            : emulatorType;
        const QString autodetection = device->isAutodetected()
            ? autodetected
            : userDefined;
        const QString privateKeyFile = FilePath::fromString(
                device->sshParameters().privateKeyFile)
            .shortNativePath();

        qout() << '#' << index << ' ' << '"' << device->name() << '"' << endl;
        qout() << indent(1) << qSetFieldWidth(typeFieldWidth) << left << type << qSetFieldWidth(0)
            << "  " << qSetFieldWidth(autodetectedFieldWidth) << autodetection << qSetFieldWidth(0)
            << "  " << device->sshParameters().url.authority() << endl;
        qout() << indent(1) << tr("private-key:") << ' ' << privateKeyFile << endl;

        ++index;
    }
}

Device *BuiltinWorker::deviceForNameOrIndex(const QString &deviceNameOrIndex,
        QString *errorString)
{
    bool isInt;
    const int deviceIndex = deviceNameOrIndex.toInt(&isInt);
    if (isInt) {
        if (deviceIndex < 0 || deviceIndex > Sdk::devices().count() - 1) {
            *errorString = tr("Invalid device index: %1").arg(deviceNameOrIndex);
            return nullptr;
        }
        return Sdk::devices().at(deviceIndex);
    } else {
        return SdkManager::deviceByName(deviceNameOrIndex, errorString);
    }
}

bool BuiltinWorker::listEmulators(SdkManager::ListEmulatorsOptions options)
{
    QList<EmulatorInfo> infoList;
    const bool ok = SdkManager::listEmulators(options, &infoList);
    if (!ok)
        return false;

    const bool saySdkProvided = !(options & SdkManager::AvailableEmulators);
    const bool indicateDefault = !(options & SdkManager::AvailableEmulators);

    QList<QStringList> table;
    for (const EmulatorInfo &info : infoList)
        table << QStringList{info.name, {}, toString(info.flags, saySdkProvided, indicateDefault)};

    TreePrinter::Tree tree = TreePrinter::build(table, 0, 1);
    TreePrinter::sort(&tree, 0, 0, true);
    TreePrinter::print(qout(), tree, {0, 2});

    return true;
}

Emulator *BuiltinWorker::defaultEmulator(QString *errorString)
{
    if (Sdk::emulators().isEmpty()) {
        *errorString = tr("No emulator available");
        return nullptr;
    }

    // Ordering forced in EmulatorManager::fromMap - the most recent one comes first
    return Sdk::emulators().first();
}

void BuiltinWorker::listDeviceModels()
{
    for (const DeviceModelData &model : Sdk::deviceModels())
        qout() << model.name << endl;
}

bool BuiltinWorker::showDeviceModel(const QString &name)
{
    const DeviceModelData model = Sdk::deviceModel(name);
    if (!model.isValid()) {
        qerr() << tr("%1: No such device model").arg(name) << endl;
        return false;
    }

    qout() << tr("display-resolution") << ": " << QString("%1x%2 px")
            .arg(model.displayResolution.width())
            .arg(model.displayResolution.height());
    qout() << endl;
    qout() << tr("display-size") << ": " << QString("%1x%2 mm")
            .arg(model.displaySize.width())
            .arg(model.displaySize.height());
    qout() << endl;
    qout() << tr("dconf-properties") << ":" << endl;
    qout() << indentLines(1, model.dconf.trimmed());
    qout() << endl;

    return true;
}

bool BuiltinWorker::listTools(SdkManager::ListToolsOptions options, bool listToolings,
        bool listTargets)
{
    QList<ToolsInfo> infoList;
    const bool ok = SdkManager::listTools(options, &infoList);
    if (!ok)
        return false;

    const bool saySdkProvided = !(options & SdkManager::AvailableTools);

    QList<QStringList> table;
    for (const ToolsInfo &info : infoList) {
        const QString flags = toString(info.flags, saySdkProvided);
        if (info.flags & ToolsInfo::Tooling) {
            if (!listToolings)
                continue;
            table << QStringList{info.name, info.parentName, flags};
        } else {
            if (!listTargets)
                continue;
            if (info.flags & ToolsInfo::Snapshot)
                table << QStringList{info.name, info.parentName, flags};
            else if (listToolings)
                table << QStringList{info.name, info.parentName, flags};
            else
                table << QStringList{info.name, {}, flags};
        }
    }

    TreePrinter::Tree tree = TreePrinter::build(table, 0, 1);
    TreePrinter::sort(&tree, 0, true);
    TreePrinter::print(qout(), tree, {0, 2});

    return true;
}

bool BuiltinWorker::stopVirtualMachines()
{
    for (BuildEngine *const engine : Sdk::buildEngines()) {
        if (!SdkManager::stopReliably(engine->virtualMachine())) {
            qerr() << tr("Failed to stop the build engine \"%1\"").arg(engine->name()) << endl;
            return false;
        }
    }

    for (Emulator *const emulator : Sdk::emulators()) {
        if (!SdkManager::stopReliably(emulator->virtualMachine())) {
            qerr() << tr("Failed to stop the emulator \"%1\"").arg(emulator->name()) << endl;
            return false;
        }
    }

    return true;
}

void BuiltinWorker::printProperties(const PropertiesAccessor &accessor)
{
    QMap<QString, QString> properties = accessor.get();
    for (auto it = properties.cbegin(); it != properties.cend(); ++it)
        qout() << it.key() << ':' << ' ' << it.value() << endl;
}

Worker::ExitStatus BuiltinWorker::setProperties(SetPropertiesTask *task,
        const QStringList &assignments, int *exitCode)
{
    for (const QString &assignment : assignments) {
        const int splitAt = assignment.indexOf('=');
        if (splitAt <= 0) {
            qerr() << tr("Assignment expected: \"%1\"").arg(assignment) << endl;
            return BadUsage;
        }
        const QString property = assignment.left(splitAt);
        const QString value = assignment.mid(splitAt + 1);

        // CamelCase to snake_case for backward compatibility
        const QString normalizedProperty = QString(property)
            .replace(QRegularExpression("([A-Z])"), "-\\1").toLower();

        QString errorString;
        if (!task->prepareSet(normalizedProperty, value, &errorString)) {
            *exitCode = EXIT_FAILURE;
            qerr() << property << ": " << errorString << endl;
            return NormalExit;
        }

        if (normalizedProperty != property) {
            qCInfo(sfdk).noquote() << tr("The \"%1\" property is deprecated. Use \"%2\" instead.")
                .arg(property).arg(normalizedProperty);
        }
    }

    QString errorString;
    if (!task->set(&errorString)) {
        *exitCode = EXIT_FAILURE;
        qerr() << errorString << endl;
        return NormalExit;
    }

    *exitCode = EXIT_SUCCESS;
    return NormalExit;
}

QString BuiltinWorker::runningYesNoMessage(bool running)
{
    return tr("running: %1").arg(running ? tr("yes") : tr("no"));
}

QString BuiltinWorker::toString(ToolsInfo::Flags flags, bool saySdkProvided)
{
    QStringList keywords;

    // The order matters.
    // The two flags Tooling and Target are intentionally not reflected in the output
    if (flags & ToolsInfo::Available)
        keywords << SdkManager::stateAvailableMessage();
    if (flags & ToolsInfo::Installed) {
        if (saySdkProvided)
            keywords << SdkManager::stateSdkProvidedMessage();
        else
            keywords << SdkManager::stateInstalledMessage();
    }
    if (flags & ToolsInfo::UserDefined)
        keywords << SdkManager::stateUserDefinedMessage();
    if (flags & ToolsInfo::Snapshot)
        keywords << tr("snapshot");
    if (flags & ToolsInfo::Outdated)
        keywords << tr("outdated");
    if (flags & ToolsInfo::Latest)
        keywords << SdkManager::stateLatestMessage();
    if (flags & ToolsInfo::EarlyAccess)
        keywords << SdkManager::stateEarlyAccessMessage();

    return keywords.join(',');
}

QString BuiltinWorker::toString(EmulatorInfo::Flags flags, bool saySdkProvided, bool indicateDefault)
{
    QStringList keywords;

    // The order matters.
    if (flags & EmulatorInfo::Available)
        keywords << SdkManager::stateAvailableMessage();
    if (flags & EmulatorInfo::Installed) {
        if (saySdkProvided)
            keywords << SdkManager::stateSdkProvidedMessage();
        else
            keywords << SdkManager::stateInstalledMessage();
    }
    if (flags & EmulatorInfo::Latest)
        keywords << SdkManager::stateLatestMessage();
    if (flags & EmulatorInfo::EarlyAccess)
        keywords << SdkManager::stateEarlyAccessMessage();
    if (indicateDefault && flags & EmulatorInfo::Default)
        keywords << SdkManager::stateDefaultMessage();

    return keywords.join(',');
}

/*!
 * \class EngineWorker
 */

Worker::ExitStatus EngineWorker::doRun(const Command *command, const QStringList &arguments,
        int *exitCode) const
{
    Q_ASSERT(exitCode != nullptr);

    if (!SdkManager::hasEngine()) {
        qerr() << SdkManager::noEngineFoundMessage() << endl;
        *exitCode = SFDK_EXIT_ABNORMAL;
        return NormalExit;
    }

    QString errorString;

    QStringList globalArguments;
    if (!makeGlobalArguments(command, &globalArguments, &errorString)) {
        qerr() << errorString << endl;
        *exitCode = SFDK_EXIT_ABNORMAL;
        return BadUsage;
    }

    QStringList allArguments;
    allArguments += m_initialArguments;
    allArguments += globalArguments;
    if (!m_omitSubcommand)
        allArguments += command->name;
    allArguments += arguments;

    QProcessEnvironment extraEnvironment;

    CMakeHelper::maybePrepareCMakeApiPathMapping(&extraEnvironment);

    qCDebug(sfdk) << "About to run on build engine:" << m_program << "arguments:" << allArguments;
    *exitCode = SdkManager::runOnEngine(m_program, allArguments, extraEnvironment);

    maybeDoQtCreatorDeploymentTxtMapping();
    CMakeHelper::maybeDoCMakeApiPathMapping();

    return NormalExit;
}

std::unique_ptr<Worker> EngineWorker::fromMap(const QVariantMap &data, int version,
        QString *errorString)
{
    if (!checkVersion(version, 4, 5, errorString))
        return {};

    if (!Dispatcher::checkKeys(data, {PROGRAM_KEY, INITIAL_ARGUMENTS_KEY, OMIT_SUBCOMMAND_KEY,
                    OPTION_FORMATTER_KEY},
                errorString)) {
        return {};
    }

    auto worker = std::make_unique<EngineWorker>();

    QVariant program = Dispatcher::value(data, PROGRAM_KEY, QVariant::String, {}, errorString);
    if (!program.isValid())
        return {};
    worker->m_program = program.toString();

    QVariant initialArguments = Dispatcher::value(data, INITIAL_ARGUMENTS_KEY, QVariant::List,
            QStringList(), errorString);
    if (!initialArguments.isValid())
        return {};
    if (!Dispatcher::checkItems(initialArguments.toList(), QVariant::String, errorString))
        return {};
    worker->m_initialArguments = initialArguments.toStringList();

    QVariant omitCommand = Dispatcher::value(data, OMIT_SUBCOMMAND_KEY, QVariant::Bool, false,
            errorString);
    worker->m_omitSubcommand = omitCommand.toBool();

    QVariant optionFormatter = Dispatcher::value(data, OPTION_FORMATTER_KEY, QVariant::String,
            QString(), errorString);
    worker->m_optionFormatterJSFunctionName = optionFormatter.toString();

#ifdef Q_OS_MACOS
    return std::move(worker);
#else
    return worker;
#endif
}

bool EngineWorker::makeGlobalArguments(const Command *command, QStringList *arguments,
        QString *errorString) const
{
    auto unsetRequiredOptions = command->mandatoryConfigOptions.toSet();

    for (const OptionEffectiveOccurence &occurence : Configuration::effectiveState()) {
        if (occurence.isMasked())
            continue;
        if (command->configOptions.contains(occurence.option())
                // Do not require modules to deal with the hooks-dir option explicitly
                || (occurence.option()->name == Constants::HOOKS_DIR_OPTION_NAME
                    && !command->hooks.isEmpty())) {
            *arguments << makeGlobalArguments(command, occurence);
            unsetRequiredOptions.remove(occurence.option());
        }
    }

    if (!unsetRequiredOptions.isEmpty()) {
        *errorString = tr("The required configuration option '%1' is not set")
            .arg((*unsetRequiredOptions.cbegin())->name);
        return false;
    }

    return true;
}

QStringList EngineWorker::makeGlobalArguments(const Command *command,
        const OptionEffectiveOccurence &optionOccurence) const
{
    QStringList arguments;

    const QString normalizedName = QString(optionOccurence.option()->name).replace('.', '-');
    if (optionOccurence.argument().isEmpty()) {
        arguments << "--" + normalizedName;
    } else if (optionOccurence.option()->argumentType == Option::MandatoryArgument) {
        arguments << "--" + normalizedName << optionOccurence.argument();
    } else {
        arguments << "--" + normalizedName + "=" + optionOccurence.argument();
    }

    maybeMakeCustomGlobalArguments(command, optionOccurence, &arguments);

    return arguments;
}

void EngineWorker::maybeMakeCustomGlobalArguments(const Command *command,
        const OptionEffectiveOccurence &optionOccurence, QStringList *arguments) const
{
    if (m_optionFormatterJSFunctionName.isEmpty())
        return;

    auto resultTypeValidator = [](const QJSValue &value, QString *errorString) {
        if (!value.isArray()
                || value.property("length").toInt() < 2
                || !value.property(0).isBool()
                || !value.property(1).isArray()) {
            *errorString = "Not an array [bool, array]";
            return false;
        }

        QJSValue argumentsArray = value.property(1);
        const int length = argumentsArray.property("length").toInt();
        for (int i = 0; i < length; ++i) {
            if (!argumentsArray.property(i).isString()) {
                *errorString = "The nested array is not an array of strings";
                return false;
            }
        }

        return true;
    };

    QJSValueList formatterArguments{
        optionOccurence.option()->name,
        optionOccurence.argument(),
        optionOccurence.option()->argumentType == Option::MandatoryArgument,
        Dispatcher::jsEngine()->toScriptValue(*arguments)
    };

    const QJSValue result = Dispatcher::jsEngine()->call(m_optionFormatterJSFunctionName,
            formatterArguments, command->module, resultTypeValidator);

    if (result.isError()) {
        qCCritical(sfdk) << "Error formatting option as argument:" << result.toString();
        return;
    }

    const bool formatted = result.property(0).toBool();
    if (!formatted)
        return;

    *arguments = Dispatcher::jsEngine()->fromScriptValue<QStringList>(result.property(1));
}

void EngineWorker::maybeDoQtCreatorDeploymentTxtMapping() const
{
    if (!QFile::exists(QT_CREATOR_DEPLOYMENT_TXT))
        return;

    QString errorMessage;

    QTC_ASSERT(SdkManager::hasEngine(), return);
    BuildEngine *const engine = SdkManager::engine();

    QTC_CHECK(!engine->sharedSrcPath().toString().contains('\\'));

    bool ok;

    FileReader reader;
    ok = reader.fetch(QT_CREATOR_DEPLOYMENT_TXT);
    if (!ok) {
        qCCritical(sfdk).noquote() << reader.errorString();
        return;
    }

    QString data = QString::fromUtf8(reader.data());

    data.replace(engine->sharedSrcMountPoint(), engine->sharedSrcPath().toString());

    FileSaver saver(QT_CREATOR_DEPLOYMENT_TXT);
    saver.write(data.toUtf8());
    ok = saver.finalize();
    if (!ok) {
        qCCritical(sfdk).noquote() << saver.errorString();
        return;
    }
}

#include "command.moc"
