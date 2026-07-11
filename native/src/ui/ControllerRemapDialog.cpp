#include "ControllerRemapDialog.h"
#include "../input/Gamepad.h"
#include "../input/Keymap.h"
#include "../core/Settings.h"
#include "../core/SystemCatalog.h"
#include "nav/Nav.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QDialogButtonBox>
#include <QTimer>
#include <QKeyEvent>

// RetroPad buttons in a sensible editing order. retroId values are the stable RETRO_DEVICE_ID_JOYPAD_*
// ABI numbers; label is the name shown to the player (NES/SNES-style RetroPad naming).
namespace {
struct Row { int retroId; const char* label; };
const Row kRows[] = {
    { 4, "D-Pad Up" }, { 5, "D-Pad Down" }, { 6, "D-Pad Left" }, { 7, "D-Pad Right" },
    { 8, "A" },        { 0, "B" },          { 9, "X" },          { 1, "Y" },
    { 10, "L" },       { 11, "R" },         { 12, "L2" },        { 13, "R2" },
    { 14, "L3" },      { 15, "R3" },        { 2, "Select" },     { 3, "Start" },
};
}

ControllerRemapDialog::ControllerRemapDialog(Gamepad* pad, Keymap* keys, QWidget* parent)
    : QDialog(parent), pad_(pad), keys_(keys)
{
    setWindowTitle(tr("Input Mapping"));

    for (int p = 0; p < kPlayers; ++p)
        for (int i = 0; i < 16; ++i)
        {
            workingPad_[p][i] = pad_ ? pad_->binding(p, i) : Gamepad::defaultBinding(i);
            workingKey_[p][i] = keys_ ? keys_->key(p, i) : Keymap::defaultKey(p, i);
            workingTurbo_[p][i] = Settings::turboButton(p, i);
        }

    scope_ = Settings::inputScope();

    auto* v = new QVBoxLayout(this);

    // Profile selector: edit the global default, or a per-console override that only applies to that system.
    auto* scopeRow = new QHBoxLayout();
    scopeRow->addWidget(new QLabel(tr("Profile:"), this));
    scopeCombo_ = new QComboBox(this);
    scopeCombo_->addItem(tr("All systems (default)"), QString());
    for (const GameSystem& s : SystemCatalog::systems()) scopeCombo_->addItem(s.name, s.id);
    { const int i = scopeCombo_->findData(scope_); scopeCombo_->setCurrentIndex(i >= 0 ? i : 0); }
    connect(scopeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, &ControllerRemapDialog::onScopeChanged);
    scopeRow->addWidget(scopeCombo_, 1);
    v->addLayout(scopeRow);

    // Player selector: the controller and keyboard columns both edit this player's profile.
    auto* playerRow = new QHBoxLayout();
    playerRow->addWidget(new QLabel(tr("Editing player:"), this));
    playerCombo_ = new QComboBox(this);
    for (int p = 0; p < kPlayers; ++p) playerCombo_->addItem(tr("Player %1").arg(p + 1));
    connect(playerCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, &ControllerRemapDialog::onPlayerChanged);
    playerRow->addWidget(playerCombo_);
    playerRow->addSpacing(16);
    playerRow->addWidget(new QLabel(tr("Turbo speed:"), this));
    turboSpeed_ = new QComboBox(this);
    turboSpeed_->addItem(tr("Slow"), 5);   // half-period in frames (smaller = faster)
    turboSpeed_->addItem(tr("Medium"), 3);
    turboSpeed_->addItem(tr("Fast"), 2);
    turboSpeed_->addItem(tr("Ultra"), 1);
    {
        const int hp = Settings::turboHalfPeriod();
        const int idx = turboSpeed_->findData(hp);
        turboSpeed_->setCurrentIndex(idx >= 0 ? idx : 1);
    }
    playerRow->addWidget(turboSpeed_);
    playerRow->addStretch(1);
    v->addLayout(playerRow);

    // This dialog runs ring-off (it grabs the keyboard for bind capture), so its dropdowns don't get the
    // nav ring's auto-attach — give them the two-state select/open behaviour explicitly, so arrowing onto
    // the Profile/Player/Turbo dropdowns navigates away instead of changing the value.
    NavCombo::ensure(scopeCombo_);
    NavCombo::ensure(playerCombo_);
    NavCombo::ensure(turboSpeed_);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    v->addWidget(status_);

    auto* grid = new QGridLayout();
    grid->addWidget(new QLabel(tr("Button"), this),     0, 0);
    grid->addWidget(new QLabel(tr("Controller"), this), 0, 1);
    grid->addWidget(new QLabel(tr("Keyboard"), this),   0, 2);
    grid->addWidget(new QLabel(tr("Turbo"), this),      0, 3);

    int row = 1;
    for (const Row& r : kRows)
    {
        const int id = r.retroId;
        grid->addWidget(new QLabel(tr(r.label), this), row, 0);

        auto* padBtn = new QPushButton(this);
        padBtn->setMinimumWidth(150);
        connect(padBtn, &QPushButton::clicked, this, [this, id] { beginPadCapture(id); });
        padButtons_.insert(id, padBtn);
        grid->addWidget(padBtn, row, 1);

        auto* keyBtn = new QPushButton(this);
        keyBtn->setMinimumWidth(130);
        connect(keyBtn, &QPushButton::clicked, this, [this, id] { beginKeyCapture(id); });
        keyButtons_.insert(id, keyBtn);
        grid->addWidget(keyBtn, row, 2);

        auto* turbo = new QCheckBox(this);
        connect(turbo, &QCheckBox::toggled, this, [this, id](bool on) { workingTurbo_[port_][id] = on; });
        turboChecks_.insert(id, turbo);
        grid->addWidget(turbo, row, 3, Qt::AlignCenter);

        refreshRow(id);
        ++row;
    }
    v->addLayout(grid);

    auto* reset = new QPushButton(tr("Reset to Defaults"), this);
    connect(reset, &QPushButton::clicked, this, &ControllerRemapDialog::resetDefaults);
    v->addWidget(reset);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &ControllerRemapDialog::save);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(box);

    // Poll the controller while the dialog is open: keeps SDL state/hot-plug fresh and drives pad capture.
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &ControllerRemapDialog::onTick);
    timer_->start(30);
    onTick();
}

void ControllerRemapDialog::refreshRow(int retroId)
{
    if (QPushButton* b = padButtons_.value(retroId))
        b->setText(capturingPad_ == retroId ? tr("Press a button…")
                                            : QString::fromStdString(Gamepad::labelFor(workingPad_[port_][retroId])));
    if (QPushButton* b = keyButtons_.value(retroId))
        b->setText(capturingKey_ == retroId ? tr("Press a key…")
                                            : Keymap::labelFor(workingKey_[port_][retroId]));
    if (QCheckBox* c = turboChecks_.value(retroId))
    {
        QSignalBlocker block(c); // setChecked must not write back into workingTurbo_
        c->setChecked(workingTurbo_[port_][retroId]);
    }
}

void ControllerRemapDialog::onPlayerChanged(int index)
{
    cancelCapture();
    port_ = qBound(0, index, kPlayers - 1);
    for (const Row& r : kRows) refreshRow(r.retroId);
}

void ControllerRemapDialog::cancelCapture()
{
    const int p = capturingPad_, k = capturingKey_;
    capturingPad_ = -1;
    capturingKey_ = -1;
    if (k >= 0) releaseKeyboard();
    if (p >= 0) refreshRow(p);
    if (k >= 0) refreshRow(k);
}

void ControllerRemapDialog::beginPadCapture(int retroId)
{
    cancelCapture();
    capturingPad_ = retroId;
    sawRelease_ = false; // wait for a clean release so the click itself can't bind
    refreshRow(retroId);
}

void ControllerRemapDialog::beginKeyCapture(int retroId)
{
    cancelCapture();
    capturingKey_ = retroId;
    grabKeyboard(); // route every key press here until we bind or cancel
    refreshRow(retroId);
}

void ControllerRemapDialog::onTick()
{
    if (!pad_) return;
    pad_->poll();

    if (pad_->portConnected(port_))
    {
        const std::string n = pad_->name(port_);
        status_->setText(tr("Player %1 controller: %2")
                             .arg(port_ + 1)
                             .arg(n.empty() ? tr("connected") : QString::fromStdString(n)));
    }
    else if (pad_->connected())
        status_->setText(tr("No controller in player %1's slot — capturing from any connected controller.").arg(port_ + 1));
    else
        status_->setText(tr("No controller detected — connect one to assign controller buttons."));

    if (capturingPad_ < 0) return;
    const int code = pad_->anyPressed(port_); // prefer this player's controller
    if (!sawRelease_) { if (code == Gamepad::kUnbound) sawRelease_ = true; return; }
    if (code != Gamepad::kUnbound)
    {
        workingPad_[port_][capturingPad_] = code;
        const int done = capturingPad_;
        capturingPad_ = -1;
        refreshRow(done);
    }
}

void ControllerRemapDialog::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape)
    {
        if (capturingPad_ >= 0 || capturingKey_ >= 0) { cancelCapture(); return; }
        QDialog::keyPressEvent(e);
        return;
    }
    if (capturingKey_ >= 0)
    {
        const int id = capturingKey_;
        workingKey_[port_][id] = e->key();
        capturingKey_ = -1;
        releaseKeyboard();
        // A key drives one button within this player's profile; clear it from any other button.
        for (int i = 0; i < 16; ++i)
            if (i != id && workingKey_[port_][i] == e->key()) { workingKey_[port_][i] = Keymap::kUnbound; refreshRow(i); }
        refreshRow(id);
        return;
    }
    QDialog::keyPressEvent(e);
}

void ControllerRemapDialog::resetDefaults()
{
    cancelCapture();
    // Reset the currently-shown player's controller + keyboard + turbo settings.
    for (const Row& r : kRows)
    {
        workingPad_[port_][r.retroId] = Gamepad::defaultBinding(r.retroId);
        workingKey_[port_][r.retroId] = Keymap::defaultKey(port_, r.retroId);
        workingTurbo_[port_][r.retroId] = false;
        refreshRow(r.retroId);
    }
}

void ControllerRemapDialog::commitWorking()
{
    for (int p = 0; p < kPlayers; ++p)
        for (const Row& r : kRows)
        {
            if (pad_)  pad_->setBinding(p, r.retroId, workingPad_[p][r.retroId]);
            if (keys_) keys_->setKey(p, r.retroId, workingKey_[p][r.retroId]);
            Settings::setTurboButton(p, r.retroId, workingTurbo_[p][r.retroId]);
        }
    Settings::setTurboHalfPeriod(turboSpeed_->currentData().toInt());
}

void ControllerRemapDialog::reloadWorking()
{
    for (int p = 0; p < kPlayers; ++p)
        for (int i = 0; i < 16; ++i)
        {
            workingPad_[p][i]   = pad_ ? pad_->binding(p, i) : Gamepad::defaultBinding(i);
            workingKey_[p][i]   = keys_ ? keys_->key(p, i) : Keymap::defaultKey(p, i);
            workingTurbo_[p][i] = Settings::turboButton(p, i);
        }
}

// Switch which per-system profile is being edited: persist the current edits to the old scope, point Settings
// at the new scope, reload, and show that profile's bindings.
void ControllerRemapDialog::onScopeChanged(int index)
{
    cancelCapture();
    commitWorking();                       // keep edits made under the previous scope
    scope_ = scopeCombo_->itemData(index).toString();
    Settings::setInputScope(scope_);
    if (pad_)  pad_->reloadMapping();
    if (keys_) keys_->reload();
    reloadWorking();
    for (const Row& r : kRows) refreshRow(r.retroId);
}

void ControllerRemapDialog::save()
{
    cancelCapture();
    commitWorking();
    accept();
}
