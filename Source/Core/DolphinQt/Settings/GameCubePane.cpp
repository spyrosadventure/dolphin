// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Settings/GameCubePane.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <array>
#include <utility>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/NetPlayServer.h"

#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/GCMemcardManager.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/Settings/BroadbandAdapterSettingsDialog.h"

enum
{
  SLOT_A_INDEX,
  SLOT_B_INDEX,
  SLOT_SP1_INDEX,
  SLOT_COUNT
};

GameCubePane::GameCubePane()
{
  CreateWidgets();
  LoadSettings();
  ConnectWidgets();
}

void GameCubePane::CreateWidgets()
{
  using ExpansionInterface::EXIDeviceType;

  QVBoxLayout* layout = new QVBoxLayout(this);

  // IPL Settings
  QGroupBox* ipl_box = new QGroupBox(tr("IPL Settings"), this);
  QVBoxLayout* ipl_box_layout = new QVBoxLayout(ipl_box);
  ipl_box->setLayout(ipl_box_layout);

  m_skip_main_menu = new QCheckBox(tr("Skip Main Menu"), ipl_box);
  ipl_box_layout->addWidget(m_skip_main_menu);

  QFormLayout* ipl_language_layout = new QFormLayout;
  ipl_language_layout->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  ipl_language_layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  ipl_box_layout->addLayout(ipl_language_layout);

  m_language_combo = new QComboBox(ipl_box);
  m_language_combo->setCurrentIndex(-1);
  ipl_language_layout->addRow(tr("System Language:"), m_language_combo);

  // Add languages
  for (const auto& entry : {std::make_pair(tr("English"), 0), std::make_pair(tr("German"), 1),
                            std::make_pair(tr("French"), 2), std::make_pair(tr("Spanish"), 3),
                            std::make_pair(tr("Italian"), 4), std::make_pair(tr("Dutch"), 5)})
  {
    m_language_combo->addItem(entry.first, entry.second);
  }

  // Device Settings
  QGroupBox* device_box = new QGroupBox(tr("Device Settings"), this);
  QGridLayout* device_layout = new QGridLayout(device_box);
  device_box->setLayout(device_layout);

  for (int i = 0; i < SLOT_COUNT; i++)
  {
    m_slot_combos[i] = new QComboBox(device_box);
    m_slot_combos[i]->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_slot_buttons[i] = new QPushButton(tr("..."), device_box);
    m_slot_buttons[i]->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  }

  // Add slot devices
  for (const auto device : {EXIDeviceType::None, EXIDeviceType::Dummy, EXIDeviceType::MemoryCard,
                            EXIDeviceType::MemoryCardFolder, EXIDeviceType::Gecko,
                            EXIDeviceType::AGP, EXIDeviceType::Microphone})
  {
    const QString name = tr(fmt::format("{:n}", device).c_str());
    const int value = static_cast<int>(device);
    m_slot_combos[0]->addItem(name, value);
    m_slot_combos[1]->addItem(name, value);
  }

  // Add SP1 devices
  for (const auto device : {
           EXIDeviceType::None,
           EXIDeviceType::Dummy,
           EXIDeviceType::Ethernet,
           EXIDeviceType::EthernetXLink,
#ifdef __APPLE__
           EXIDeviceType::EthernetTapServer,
#endif
       })
  {
    m_slot_combos[2]->addItem(tr(fmt::format("{:n}", device).c_str()), static_cast<int>(device));
  }

  device_layout->addWidget(new QLabel(tr("Slot A:")), 0, 0);
  device_layout->addWidget(m_slot_combos[0], 0, 1);
  device_layout->addWidget(m_slot_buttons[0], 0, 2);
  device_layout->addWidget(new QLabel(tr("Slot B:")), 1, 0);
  device_layout->addWidget(m_slot_combos[1], 1, 1);
  device_layout->addWidget(m_slot_buttons[1], 1, 2);
  device_layout->addWidget(new QLabel(tr("SP1:")), 2, 0);
  device_layout->addWidget(m_slot_combos[2], 2, 1);
  device_layout->addWidget(m_slot_buttons[2], 2, 2);

#ifdef HAS_LIBMGBA
  // GBA Settings
  auto* gba_box = new QGroupBox(tr("GBA Settings"), this);
  auto* gba_layout = new QGridLayout(gba_box);
  gba_box->setLayout(gba_layout);
  int gba_row = 0;

  m_gba_threads = new QCheckBox(tr("Run GBA Cores in Dedicated Threads"));
  gba_layout->addWidget(m_gba_threads, gba_row, 0, 1, -1);
  gba_row++;

  m_gba_bios_edit = new QLineEdit();
  m_gba_browse_bios = new QPushButton(QStringLiteral("..."));
  gba_layout->addWidget(new QLabel(tr("BIOS:")), gba_row, 0);
  gba_layout->addWidget(m_gba_bios_edit, gba_row, 1);
  gba_layout->addWidget(m_gba_browse_bios, gba_row, 2);
  gba_row++;

  for (size_t i = 0; i < m_gba_rom_edits.size(); ++i)
  {
    m_gba_rom_edits[i] = new QLineEdit();
    m_gba_browse_roms[i] = new QPushButton(QStringLiteral("..."));
    gba_layout->addWidget(new QLabel(tr("Port %1 ROM:").arg(i + 1)), gba_row, 0);
    gba_layout->addWidget(m_gba_rom_edits[i], gba_row, 1);
    gba_layout->addWidget(m_gba_browse_roms[i], gba_row, 2);
    gba_row++;
  }

  m_gba_save_rom_path = new QCheckBox(tr("Save in Same Directory as the ROM"));
  gba_layout->addWidget(m_gba_save_rom_path, gba_row, 0, 1, -1);
  gba_row++;

  m_gba_saves_edit = new QLineEdit();
  m_gba_browse_saves = new QPushButton(QStringLiteral("..."));
  gba_layout->addWidget(new QLabel(tr("Saves:")), gba_row, 0);
  gba_layout->addWidget(m_gba_saves_edit, gba_row, 1);
  gba_layout->addWidget(m_gba_browse_saves, gba_row, 2);
  gba_row++;
#endif

  layout->addWidget(ipl_box);
  layout->addWidget(device_box);
#ifdef HAS_LIBMGBA
  layout->addWidget(gba_box);
#endif

  layout->addStretch();

  setLayout(layout);
}

void GameCubePane::ConnectWidgets()
{
  // IPL Settings
  connect(m_skip_main_menu, &QCheckBox::stateChanged, this, &GameCubePane::SaveSettings);
  connect(m_language_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &GameCubePane::SaveSettings);

  // Device Settings
  for (int i = 0; i < SLOT_COUNT; i++)
  {
    connect(m_slot_combos[i], qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, i] { UpdateButton(i); });
    connect(m_slot_combos[i], qOverload<int>(&QComboBox::currentIndexChanged), this,
            &GameCubePane::SaveSettings);
    connect(m_slot_buttons[i], &QPushButton::clicked, [this, i] { OnConfigPressed(i); });
  }

#ifdef HAS_LIBMGBA
  // GBA Settings
  connect(m_gba_threads, &QCheckBox::stateChanged, this, &GameCubePane::SaveSettings);
  connect(m_gba_bios_edit, &QLineEdit::editingFinished, this, &GameCubePane::SaveSettings);
  connect(m_gba_browse_bios, &QPushButton::clicked, this, &GameCubePane::BrowseGBABios);
  connect(m_gba_save_rom_path, &QCheckBox::stateChanged, this, &GameCubePane::SaveRomPathChanged);
  connect(m_gba_saves_edit, &QLineEdit::editingFinished, this, &GameCubePane::SaveSettings);
  connect(m_gba_browse_saves, &QPushButton::clicked, this, &GameCubePane::BrowseGBASaves);
  for (size_t i = 0; i < m_gba_browse_roms.size(); ++i)
  {
    connect(m_gba_rom_edits[i], &QLineEdit::editingFinished, this, &GameCubePane::SaveSettings);
    connect(m_gba_browse_roms[i], &QPushButton::clicked, this, [this, i] { BrowseGBARom(i); });
  }
#endif

  // Emulation State
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &GameCubePane::OnEmulationStateChanged);
  OnEmulationStateChanged();
}

void GameCubePane::OnEmulationStateChanged()
{
#ifdef HAS_LIBMGBA
  bool gba_enabled = !NetPlay::IsNetPlayRunning();
  m_gba_threads->setEnabled(gba_enabled);
  m_gba_bios_edit->setEnabled(gba_enabled);
  m_gba_browse_bios->setEnabled(gba_enabled);
  m_gba_save_rom_path->setEnabled(gba_enabled);
  m_gba_saves_edit->setEnabled(gba_enabled);
  m_gba_browse_saves->setEnabled(gba_enabled);
  for (size_t i = 0; i < m_gba_browse_roms.size(); ++i)
  {
    m_gba_rom_edits[i]->setEnabled(gba_enabled);
    m_gba_browse_roms[i]->setEnabled(gba_enabled);
  }
#endif
}

void GameCubePane::UpdateButton(int slot)
{
  const auto device =
      static_cast<ExpansionInterface::EXIDeviceType>(m_slot_combos[slot]->currentData().toInt());
  bool has_config = false;

  switch (slot)
  {
  case SLOT_A_INDEX:
  case SLOT_B_INDEX:
    has_config = (device == ExpansionInterface::EXIDeviceType::MemoryCard ||
                  device == ExpansionInterface::EXIDeviceType::AGP ||
                  device == ExpansionInterface::EXIDeviceType::Microphone);
    break;
  case SLOT_SP1_INDEX:
    has_config = (device == ExpansionInterface::EXIDeviceType::Ethernet ||
                  device == ExpansionInterface::EXIDeviceType::EthernetXLink);
    break;
  }

  m_slot_buttons[slot]->setEnabled(has_config);
}

void GameCubePane::OnConfigPressed(int slot)
{
  QString filter;
  bool memcard = false;

  const ExpansionInterface::EXIDeviceType device =
      static_cast<ExpansionInterface::EXIDeviceType>(m_slot_combos[slot]->currentData().toInt());

  switch (device)
  {
  case ExpansionInterface::EXIDeviceType::MemoryCard:
    filter = tr("GameCube Memory Cards (*.raw *.gcp)");
    memcard = true;
    break;
  case ExpansionInterface::EXIDeviceType::AGP:
    filter = tr("Game Boy Advance Carts (*.gba)");
    break;
  case ExpansionInterface::EXIDeviceType::Microphone:
    MappingWindow(this, MappingWindow::Type::MAPPING_GC_MICROPHONE, slot).exec();
    return;
  case ExpansionInterface::EXIDeviceType::Ethernet:
  {
    BroadbandAdapterSettingsDialog(this, BroadbandAdapterSettingsDialog::Type::Ethernet).exec();
    return;
  }
  case ExpansionInterface::EXIDeviceType::EthernetXLink:
  {
    BroadbandAdapterSettingsDialog(this, BroadbandAdapterSettingsDialog::Type::XLinkKai).exec();
    return;
  }
  default:
    PanicAlertFmt("Unknown settings pressed for {}", device);
    return;
  }

  QString filename = DolphinFileDialog::getSaveFileName(
      this, tr("Choose a file to open"), QString::fromStdString(File::GetUserPath(D_GCUSER_IDX)),
      filter, 0, QFileDialog::DontConfirmOverwrite);

  if (filename.isEmpty())
    return;

  QString path_abs = QFileInfo(filename).absoluteFilePath();

  // Memcard validity checks
  if (memcard)
  {
    if (File::Exists(filename.toStdString()))
    {
      auto [error_code, mc] = Memcard::GCMemcard::Open(filename.toStdString());

      if (error_code.HasCriticalErrors() || !mc || !mc->IsValid())
      {
        ModalMessageBox::critical(
            this, tr("Error"),
            tr("The file\n%1\nis either corrupted or not a GameCube memory card file.\n%2")
                .arg(filename)
                .arg(GCMemcardManager::GetErrorMessagesForErrorCode(error_code)));
        return;
      }
    }

    bool other_slot_memcard =
        m_slot_combos[slot == SLOT_A_INDEX ? SLOT_B_INDEX : SLOT_A_INDEX]->currentData().toInt() ==
        static_cast<int>(ExpansionInterface::EXIDeviceType::MemoryCard);
    if (other_slot_memcard)
    {
      QString path_b =
          QFileInfo(QString::fromStdString(slot == 0 ? Config::Get(Config::MAIN_MEMCARD_B_PATH) :
                                                       Config::Get(Config::MAIN_MEMCARD_A_PATH)))
              .absoluteFilePath();

      if (path_abs == path_b)
      {
        ModalMessageBox::critical(this, tr("Error"),
                                  tr("The same file can't be used in both slots."));
        return;
      }
    }
  }

  QString path_old;
  if (memcard)
  {
    path_old =
        QFileInfo(QString::fromStdString(slot == 0 ? Config::Get(Config::MAIN_MEMCARD_A_PATH) :
                                                     Config::Get(Config::MAIN_MEMCARD_B_PATH)))
            .absoluteFilePath();
  }
  else
  {
    path_old =
        QFileInfo(QString::fromStdString(slot == 0 ? Config::Get(Config::MAIN_AGP_CART_A_PATH) :
                                                     Config::Get(Config::MAIN_AGP_CART_B_PATH)))
            .absoluteFilePath();
  }

  if (memcard)
  {
    if (slot == SLOT_A_INDEX)
    {
      Config::SetBase(Config::MAIN_MEMCARD_A_PATH, path_abs.toStdString());
    }
    else
    {
      Config::SetBase(Config::MAIN_MEMCARD_B_PATH, path_abs.toStdString());
    }
  }
  else
  {
    if (slot == SLOT_A_INDEX)
    {
      Config::SetBase(Config::MAIN_AGP_CART_A_PATH, path_abs.toStdString());
    }
    else
    {
      Config::SetBase(Config::MAIN_AGP_CART_B_PATH, path_abs.toStdString());
    }
  }

  if (Core::IsRunning() && path_abs != path_old)
  {
    ExpansionInterface::ChangeDevice(
        // SlotB is on channel 1, slotA and SP1 are on 0
        slot,
        // The device enum to change to
        memcard ? ExpansionInterface::EXIDeviceType::MemoryCard :
                  ExpansionInterface::EXIDeviceType::AGP,
        // SP1 is device 2, slots are device 0
        0);
  }
}

void GameCubePane::BrowseGBABios()
{
  QString file = QDir::toNativeSeparators(DolphinFileDialog::getOpenFileName(
      this, tr("Select GBA BIOS"), QString::fromStdString(File::GetUserPath(F_GBABIOS_IDX)),
      tr("All Files (*)")));
  if (!file.isEmpty())
  {
    m_gba_bios_edit->setText(file);
    SaveSettings();
  }
}

void GameCubePane::BrowseGBARom(size_t index)
{
  QString file = QString::fromStdString(GetOpenGBARom({}));
  if (!file.isEmpty())
  {
    m_gba_rom_edits[index]->setText(file);
    SaveSettings();
  }
}

void GameCubePane::SaveRomPathChanged()
{
  m_gba_saves_edit->setEnabled(!m_gba_save_rom_path->isChecked());
  m_gba_browse_saves->setEnabled(!m_gba_save_rom_path->isChecked());
  SaveSettings();
}

void GameCubePane::BrowseGBASaves()
{
  QString dir = QDir::toNativeSeparators(DolphinFileDialog::getExistingDirectory(
      this, tr("Select GBA Saves Path"),
      QString::fromStdString(File::GetUserPath(D_GBASAVES_IDX))));
  if (!dir.isEmpty())
  {
    m_gba_saves_edit->setText(dir);
    SaveSettings();
  }
}

void GameCubePane::LoadSettings()
{
  // IPL Settings
  m_skip_main_menu->setChecked(Config::Get(Config::MAIN_SKIP_IPL));
  m_language_combo->setCurrentIndex(
      m_language_combo->findData(Config::Get(Config::MAIN_GC_LANGUAGE)));

  bool have_menu = false;

  for (const std::string dir : {USA_DIR, JAP_DIR, EUR_DIR})
  {
    const auto path = DIR_SEP + dir + DIR_SEP GC_IPL;
    if (File::Exists(File::GetUserPath(D_GCUSER_IDX) + path) ||
        File::Exists(File::GetSysDirectory() + GC_SYS_DIR + path))
    {
      have_menu = true;
      break;
    }
  }

  m_skip_main_menu->setEnabled(have_menu);
  m_skip_main_menu->setToolTip(have_menu ? QString{} : tr("Put IPL ROMs in User/GC/<region>."));

  // Device Settings
  for (int i = 0; i < SLOT_COUNT; i++)
  {
    QSignalBlocker blocker(m_slot_combos[i]);
    const ExpansionInterface::EXIDeviceType exi_device =
        Config::Get(Config::GetInfoForEXIDevice(static_cast<ExpansionInterface::Slot>(i)));
    m_slot_combos[i]->setCurrentIndex(m_slot_combos[i]->findData(static_cast<int>(exi_device)));
    UpdateButton(i);
  }

#ifdef HAS_LIBMGBA
  // GBA Settings
  m_gba_threads->setChecked(Config::Get(Config::MAIN_GBA_THREADS));
  m_gba_bios_edit->setText(QString::fromStdString(File::GetUserPath(F_GBABIOS_IDX)));
  m_gba_save_rom_path->setChecked(Config::Get(Config::MAIN_GBA_SAVES_IN_ROM_PATH));
  m_gba_saves_edit->setText(QString::fromStdString(File::GetUserPath(D_GBASAVES_IDX)));
  for (size_t i = 0; i < m_gba_rom_edits.size(); ++i)
    m_gba_rom_edits[i]->setText(QString::fromStdString(Config::Get(Config::MAIN_GBA_ROM_PATHS[i])));
#endif
}

void GameCubePane::SaveSettings()
{
  Config::ConfigChangeCallbackGuard config_guard;

  // IPL Settings
  Config::SetBaseOrCurrent(Config::MAIN_SKIP_IPL, m_skip_main_menu->isChecked());
  Config::SetBaseOrCurrent(Config::MAIN_GC_LANGUAGE, m_language_combo->currentData().toInt());

  // Device Settings
  for (int i = 0; i < SLOT_COUNT; i++)
  {
    const auto dev =
        static_cast<ExpansionInterface::EXIDeviceType>(m_slot_combos[i]->currentData().toInt());
    const ExpansionInterface::EXIDeviceType current_exi_device =
        Config::Get(Config::GetInfoForEXIDevice(static_cast<ExpansionInterface::Slot>(i)));

    if (Core::IsRunning() && current_exi_device != dev)
    {
      ExpansionInterface::ChangeDevice(
          // SlotB is on channel 1, slotA and SP1 are on 0
          (i == 1) ? 1 : 0,
          // The device enum to change to
          dev,
          // SP1 is device 2, slots are device 0
          (i == 2) ? 2 : 0);
    }

    Config::SetBaseOrCurrent(Config::GetInfoForEXIDevice(static_cast<ExpansionInterface::Slot>(i)),
                             dev);
  }

#ifdef HAS_LIBMGBA
  // GBA Settings
  if (!NetPlay::IsNetPlayRunning())
  {
    Config::SetBaseOrCurrent(Config::MAIN_GBA_THREADS, m_gba_threads->isChecked());
    Config::SetBaseOrCurrent(Config::MAIN_GBA_BIOS_PATH, m_gba_bios_edit->text().toStdString());
    Config::SetBaseOrCurrent(Config::MAIN_GBA_SAVES_IN_ROM_PATH, m_gba_save_rom_path->isChecked());
    Config::SetBaseOrCurrent(Config::MAIN_GBA_SAVES_PATH, m_gba_saves_edit->text().toStdString());
    File::SetUserPath(F_GBABIOS_IDX, Config::Get(Config::MAIN_GBA_BIOS_PATH));
    File::SetUserPath(D_GBASAVES_IDX, Config::Get(Config::MAIN_GBA_SAVES_PATH));
    for (size_t i = 0; i < m_gba_rom_edits.size(); ++i)
    {
      Config::SetBaseOrCurrent(Config::MAIN_GBA_ROM_PATHS[i],
                               m_gba_rom_edits[i]->text().toStdString());
    }

    auto server = Settings::Instance().GetNetPlayServer();
    if (server)
      server->SetGBAConfig(server->GetGBAConfig(), true);
  }
#endif

  LoadSettings();
}

std::string GameCubePane::GetOpenGBARom(std::string_view title)
{
  QString caption = tr("Select GBA ROM");
  if (!title.empty())
    caption += QStringLiteral(": %1").arg(QString::fromStdString(std::string(title)));
  return QDir::toNativeSeparators(
             DolphinFileDialog::getOpenFileName(
                 nullptr, caption, QString(),
                 tr("Game Boy Advance ROMs (*.gba *.gbc *.gb *.7z *.zip *.agb *.mb *.rom *.bin);;"
                    "All Files (*)")))
      .toStdString();
}
