/*
    SPDX-FileCopyrightText: 2019 David Edmundson <davidedmundson@kde.org>
    SPDX-FileCopyrightText: 2026 Juho Ovaska <ovaska.juho@gmail.com>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#pragma once

#include "../processcore/process_attribute.h"
#include "../processcore/process_data_provider.h"

#include <memory>

class nvmlLib;
struct nvmlLibDeleter
{
    void operator()(nvmlLib *lib);
};

class nvmlDetail;
struct nvmlDetailDeleter
{
    void operator()(nvmlDetail *lib);
};

class NvidiaPlugin : public KSysGuard::ProcessDataProvider
{
    Q_OBJECT
public:
    NvidiaPlugin(QObject *parent, const QVariantList &args);
    void handleEnabledChanged(bool enabled) override;

private:
    void update() override;

    std::unique_ptr<KSysGuard::ProcessAttribute> m_usage;
    std::unique_ptr<KSysGuard::ProcessAttribute> m_memory;

    std::unique_ptr<nvmlLib, nvmlLibDeleter> m_nvmlLib;
    std::unique_ptr<nvmlDetail, nvmlDetailDeleter> m_nvmlDetail;
};
