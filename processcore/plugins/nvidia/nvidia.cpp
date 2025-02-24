/*
    SPDX-FileCopyrightText: 2019 David Edmundson <davidedmundson@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "nvidia.h"

#include <QDebug>
#include <QProcess>
#include <QStandardPaths>

#include <KLocalizedString>
#include <KPluginFactory>

#include "../processcore/process.h"

using namespace KSysGuard;

NvidiaPlugin::NvidiaPlugin(QObject *parent, const QVariantList &args)
    : ProcessDataProvider(parent, args)
    , m_sniExecutablePath(QStandardPaths::findExecutable(QStringLiteral("nvidia-smi")))
{
    if (m_sniExecutablePath.isEmpty()) {
        return;
    }

    m_usage = new ProcessAttribute(QStringLiteral("nvidia_usage"), i18n("GPU Usage"), this);
    m_usage->setUnit(KSysGuard::UnitPercent);
    m_memory = new ProcessAttribute(QStringLiteral("nvidia_memory"), i18n("GPU Memory"), this);
    m_memory->setUnit(KSysGuard::UnitPercent);

    addProcessAttribute(m_usage);
    addProcessAttribute(m_memory);
}

void NvidiaPlugin::handleEnabledChanged(bool enabled)
{
    if (enabled) {
        if (!m_process) {
            setup();
        }
        m_process->start();
    } else {
        if (m_process) {
            m_process->terminate();
        }
    }
}

void NvidiaPlugin::setup()
{
    m_process = new QProcess(this);
    m_process->setProgram(m_sniExecutablePath);
    m_process->setArguments({QStringLiteral("pmon")});

    // reset
    m_expected = m_pidIndex = m_smIndex = m_memIndex = -1;

    connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
        while (m_process->canReadLine()) {
            const QString line = QString::fromLatin1(m_process->readLine());

            // header is not parsed yet
            if (m_expected < 0) {
                // not a comment line
                if (!line.startsWith('#')) {
                    continue;
                }

                // parse header
                m_expected = -1; // header has one extra column
                for (const auto &column : QStringView(line).split(QLatin1Char(' '), Qt::SkipEmptyParts))
                {
                    // format header --> reset
                    if ((m_expected == 0) && !QString::compare(column, "idx", Qt::CaseInsensitive)) {
                        m_expected = -1;
                        break;

                    // pid
                    } else if (!QString::compare(column, "pid", Qt::CaseInsensitive)) {
                        m_pidIndex = m_expected;

                    // sm
                    } else if (!QString::compare(column, "sm", Qt::CaseInsensitive)) {
                        m_smIndex = m_expected;

                    // mem
                    } else if (!QString::compare(column, "mem", Qt::CaseInsensitive)) {
                        m_memIndex = m_expected;
                    }

                    m_expected++;
                }

                // check that we got everything, otherwise terminate
                if ((m_expected < 3) || (m_pidIndex < 0) || (m_smIndex < 0) || (m_memIndex < 0)) {
                    m_process->terminate();
                    break;
                }

            // row indecies are known
            } else {
                const auto parts = QStringView(line).split(QLatin1Char(' '), Qt::SkipEmptyParts);

                // column count mismatch
                if (parts.count() != m_expected)
                    continue;

                long pid = parts[m_pidIndex].toUInt();
                int sm   = parts[m_smIndex].toUInt();
                int mem  = parts[m_memIndex].toUInt();

                KSysGuard::Process *process = getProcess(pid);
                if (!process) {
                    continue; // can in race condition etc
                }

                m_usage->setData(process, sm);
                m_memory->setData(process, mem);
            }
        }
    });
}

K_PLUGIN_FACTORY_WITH_JSON(PluginFactory, "nvidia.json", registerPlugin<NvidiaPlugin>();)

#include "nvidia.moc"
