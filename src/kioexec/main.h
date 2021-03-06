/* This file is part of the KDE project
   Copyright (C) 1998, 1999 Torben Weis <weis@kde.org>
   Copyright (C)  2000-2005 David Faure <faure@kde.org>
   Copyright (C)       2001 Waldo Bastian <bastian@kde.org>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/


#ifndef KIOEXEC_MAIN_H
#define KIOEXEC_MAIN_H

#include <QObject>
#include <QString>
#include <QList>
#include <QTimer>

#include <QUrl>
#include <QDateTime>

namespace KIO
{
class Job;
}

class KJob;
class QCommandLineParser;

class KIOExec : public QObject
{
    Q_OBJECT
public:
    KIOExec(const QStringList &args, bool tempFiles, const QString &suggestedFileName);

    bool exited() const
    {
        return mExited;
    }

public Q_SLOTS:
    void slotResult(KJob *);
    void slotRunApp();

protected:
    bool mExited;
    bool mTempFiles;
    bool mUseDaemon;
    QString mSuggestedFileName;
    int counter;
    int expectedCounter;
    QString command;
    struct FileInfo {
        QString path;
        QUrl url;
        QDateTime time;
    };
    QList<FileInfo> fileList;
    int jobCounter;
    QList<KIO::Job *> jobList;
};

#endif
