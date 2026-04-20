/*
    SPDX-FileCopyrightText: 2019 David Edmundson <davidedmundson@kde.org>
    SPDX-FileCopyrightText: 2026 Juho Ovaska <ovaska.juho@gmail.com>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#pragma once

#include "../processcore/process_attribute.h"
#include "../processcore/process_data_provider.h"

class nvmlLib;
class nvmlDetail;

class NvidiaPlugin : public KSysGuard::ProcessDataProvider
{
    Q_OBJECT
public:
    NvidiaPlugin(QObject *parent, const QVariantList &args);
    ~NvidiaPlugin();

    void handleEnabledChanged(bool enabled) override;

private:
    void update() override;

    KSysGuard::ProcessAttribute *m_usage = nullptr;
    KSysGuard::ProcessAttribute *m_memory = nullptr;

    std::unique_ptr<nvmlLib> m_nvmlLib;
    std::unique_ptr<nvmlDetail> m_nvmlDetail;
};
