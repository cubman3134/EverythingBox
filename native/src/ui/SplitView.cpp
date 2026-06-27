#include "SplitView.h"
#include "MediaPane.h"

#include <QSplitter>
#include <QHBoxLayout>

SplitView::SplitView(QWidget* parent) : QWidget(parent)
{
    a_ = new MediaPane(this);
    b_ = new MediaPane(this);

    split_ = new QSplitter(Qt::Horizontal, this);
    split_->setChildrenCollapsible(false);
    split_->setHandleWidth(6);
    split_->addWidget(a_);
    split_->addWidget(b_);
    split_->setSizes({ 1, 1 }); // start at an even 50/50

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(split_);

    auto wire = [this](MediaPane* p) {
        connect(p, &MediaPane::focusRequested, this, [this, p] { focusPane(p); });
        connect(p, &MediaPane::openHereRequested, this, [this, p] { focusPane(p); emit openHereRequested(p); });
        connect(p, &MediaPane::closeRequested, this, [this, p] {
            p->clear();
            if (bothEmpty()) emit exitRequested();
            else focusPane(p == a_ ? b_ : a_); // hand focus to whatever's still playing
        });
    };
    wire(a_);
    wire(b_);
    focusPane(a_);
}

void SplitView::focusPane(MediaPane* p)
{
    if (!p) return;
    focused_ = p;
    a_->setFocused(a_ == p); // only the focused pane's game takes input
    b_->setFocused(b_ == p);
}

bool SplitView::bothEmpty() const { return a_->isEmpty() && b_->isEmpty(); }

void SplitView::clearAll() { a_->clear(); b_->clear(); }
