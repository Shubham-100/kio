/**
 * Copyright (c) 2000- Dawit Alemayehu <adawit@kde.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

// Own
#include "kcookiespolicyselectiondlg.h"

// Qt
#include <QPushButton>
#include <QDialogButtonBox>
#include <QWhatsThis>
#include <QValidator>

// KDE
#include <QLineEdit>
#include <kcombobox.h>
#include <KConfigGroup>
#include <QVBoxLayout>

class DomainNameValidator : public QValidator
{
    Q_OBJECT
public:
    DomainNameValidator (QObject* parent)
        :QValidator(parent)
    {
        setObjectName(QStringLiteral("domainValidator"));
    }

    State validate (QString& input, int&) const override
    {
        if (input.isEmpty() || (input == QLatin1String("."))) {
            return Intermediate;
        }

        const int length = input.length();

        for (int i = 0 ; i < length; i++) {
            if (!input[i].isLetterOrNumber() && input[i] != '.' && input[i] != '-') {
                return Invalid;
            }
        }

        return Acceptable;
    }
};


KCookiesPolicySelectionDlg::KCookiesPolicySelectionDlg (QWidget* parent, Qt::WindowFlags flags)
    : QDialog (parent, flags)
      , mOldPolicy(-1)
      , mButtonBox(nullptr)
{
    QWidget *mainWidget = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(mainWidget);
    mUi.setupUi(mainWidget);
    mUi.leDomain->setValidator(new DomainNameValidator (mUi.leDomain));
    mUi.cbPolicy->setMinimumWidth(mUi.cbPolicy->fontMetrics().maxWidth() * 15);

    mButtonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(mButtonBox);

    connect(mButtonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(mButtonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    mButtonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(mUi.leDomain, SIGNAL(textEdited(QString)),
            SLOT(slotTextChanged(QString)));
    connect(mUi.cbPolicy, SIGNAL(currentIndexChanged(QString)),
            SLOT(slotPolicyChanged(QString)));

    mUi.leDomain->setFocus();
}

void KCookiesPolicySelectionDlg::setEnableHostEdit (bool state, const QString& host)
{
    if (!host.isEmpty()) {
        mUi.leDomain->setText (host);
        mButtonBox->button(QDialogButtonBox::Ok)->setEnabled(state);
    }

    mUi.leDomain->setEnabled (state);
}

void KCookiesPolicySelectionDlg::setPolicy (int policy)
{
    if (policy > -1 && policy <= static_cast<int> (mUi.cbPolicy->count())) {
        const bool blocked = mUi.cbPolicy->blockSignals(true);
        mUi.cbPolicy->setCurrentIndex (policy - 1);
        mUi.cbPolicy->blockSignals(blocked);
        mOldPolicy = policy;
    }

    if (!mUi.leDomain->isEnabled())
        mUi.cbPolicy->setFocus();
}

int KCookiesPolicySelectionDlg::advice () const
{
    return mUi.cbPolicy->currentIndex() + 1;
}

QString KCookiesPolicySelectionDlg::domain () const
{
    return mUi.leDomain->text();
}

void KCookiesPolicySelectionDlg::slotTextChanged (const QString& text)
{
    mButtonBox->button(QDialogButtonBox::Ok)->setEnabled (text.length() > 1);
}

void KCookiesPolicySelectionDlg::slotPolicyChanged(const QString& policyText)
{
    const int policy = KCookieAdvice::strToAdvice(policyText);
    if (!mUi.leDomain->isEnabled()) {
        mButtonBox->button(QDialogButtonBox::Ok)->setEnabled(policy != mOldPolicy);
    } else {
        slotTextChanged(policyText);
    }
}

#include "kcookiespolicyselectiondlg.moc"

