/* This file is part of the KDE libraries
    Copyright (C) 2000 Stephan Kulow <coolo@kde.org>
                  2000-2009 David Faure <faure@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#include "filecopyjob.h"
#include "job_p.h"
#include <QTimer>
#include "kprotocolmanager.h"
#include "scheduler.h"
#include "slave.h"
#include <KLocalizedString>

using namespace KIO;

static inline Slave *jobSlave(SimpleJob *job)
{
    return SimpleJobPrivate::get(job)->m_slave;
}

/** @internal */
class KIO::FileCopyJobPrivate: public KIO::JobPrivate
{
public:
    FileCopyJobPrivate(const QUrl &src, const QUrl &dest, int permissions,
                       bool move, JobFlags flags)
        : m_sourceSize(filesize_t(-1)), m_src(src), m_dest(dest), m_moveJob(nullptr), m_copyJob(nullptr), m_delJob(nullptr),
          m_chmodJob(nullptr), m_getJob(nullptr), m_putJob(nullptr), m_permissions(permissions),
          m_move(move), m_mustChmod(0), m_flags(flags)
    {
    }
    KIO::filesize_t m_sourceSize;
    QDateTime m_modificationTime;
    QUrl m_src;
    QUrl m_dest;
    QByteArray m_buffer;
    SimpleJob *m_moveJob;
    SimpleJob *m_copyJob;
    SimpleJob *m_delJob;
    SimpleJob *m_chmodJob;
    TransferJob *m_getJob;
    TransferJob *m_putJob;
    int m_permissions;
    bool m_move: 1;
    bool m_canResume: 1;
    bool m_resumeAnswerSent: 1;
    bool m_mustChmod: 1;
    JobFlags m_flags;

    void startBestCopyMethod();
    void startCopyJob();
    void startCopyJob(const QUrl &slave_url);
    void startRenameJob(const QUrl &slave_url);
    void startDataPump();
    void connectSubjob(SimpleJob *job);

    void slotStart();
    void slotData(KIO::Job *, const QByteArray &data);
    void slotDataReq(KIO::Job *, QByteArray &data);
    void slotMimetype(KIO::Job *, const QString &type);
    /**
     * Forward signal from subjob
     * @param job the job that emitted this signal
     * @param size the processed size in bytes
     */
    void slotProcessedSize(KJob *job, qulonglong size);
    /**
     * Forward signal from subjob
     * @param job the job that emitted this signal
     * @param size the total size
     */
    void slotTotalSize(KJob *job, qulonglong size);
    /**
     * Forward signal from subjob
     * @param job the job that emitted this signal
     * @param pct the percentage
     */
    void slotPercent(KJob *job, unsigned long pct);
    /**
     * Forward signal from subjob
     * @param job the job that emitted this signal
     * @param offset the offset to resume from
     */
    void slotCanResume(KIO::Job *job, KIO::filesize_t offset);

    Q_DECLARE_PUBLIC(FileCopyJob)

    static inline FileCopyJob *newJob(const QUrl &src, const QUrl &dest, int permissions, bool move,
                                      JobFlags flags)
    {
        //qDebug() << src << "->" << dest;
        FileCopyJob *job = new FileCopyJob(
            *new FileCopyJobPrivate(src, dest, permissions, move, flags));
        job->setProperty("destUrl", dest.toString());
        job->setUiDelegate(KIO::createDefaultJobUiDelegate());
        if (!(flags & HideProgressInfo)) {
            KIO::getJobTracker()->registerJob(job);
        }
        if (!(flags & NoPrivilegeExecution)) {
            job->d_func()->m_privilegeExecutionEnabled = true;
            job->d_func()->m_operationType = move ? Move : Copy;
        }
        return job;
    }
};

/*
 * The FileCopyJob works according to the famous Bavarian
 * 'Alternating Bitburger Protocol': we either drink a beer or we
 * we order a beer, but never both at the same time.
 * Translated to io-slaves: We alternate between receiving a block of data
 * and sending it away.
 */
FileCopyJob::FileCopyJob(FileCopyJobPrivate &dd)
    : Job(dd)
{
    //qDebug();
    QTimer::singleShot(0, this, SLOT(slotStart()));
}

void FileCopyJobPrivate::slotStart()
{
    Q_Q(FileCopyJob);
    if (!m_move) {
        JobPrivate::emitCopying(q, m_src, m_dest);
    } else {
        JobPrivate::emitMoving(q, m_src, m_dest);
    }

    if (m_move) {
        // The if() below must be the same as the one in startBestCopyMethod
        if ((m_src.scheme() == m_dest.scheme()) &&
                (m_src.host() == m_dest.host()) &&
                (m_src.port() == m_dest.port()) &&
                (m_src.userName() == m_dest.userName()) &&
                (m_src.password() == m_dest.password())) {
            startRenameJob(m_src);
            return;
        } else if (m_src.isLocalFile() && KProtocolManager::canRenameFromFile(m_dest)) {
            startRenameJob(m_dest);
            return;
        } else if (m_dest.isLocalFile() && KProtocolManager::canRenameToFile(m_src)) {
            startRenameJob(m_src);
            return;
        }
        // No fast-move available, use copy + del.
    }
    startBestCopyMethod();
}

void FileCopyJobPrivate::startBestCopyMethod()
{
    if ((m_src.scheme() == m_dest.scheme()) &&
            (m_src.host() == m_dest.host()) &&
            (m_src.port() == m_dest.port()) &&
            (m_src.userName() == m_dest.userName()) &&
            (m_src.password() == m_dest.password())) {
        startCopyJob();
    } else if (m_src.isLocalFile() && KProtocolManager::canCopyFromFile(m_dest)) {
        startCopyJob(m_dest);
    } else if (m_dest.isLocalFile() && KProtocolManager::canCopyToFile(m_src) &&
               !KIO::Scheduler::isSlaveOnHoldFor(m_src)) {
        startCopyJob(m_src);
    } else {
        startDataPump();
    }
}

FileCopyJob::~FileCopyJob()
{
}

void FileCopyJob::setSourceSize(KIO::filesize_t size)
{
    Q_D(FileCopyJob);
    d->m_sourceSize = size;
    if (size != (KIO::filesize_t) - 1) {
        setTotalAmount(KJob::Bytes, size);
    }
}

void FileCopyJob::setModificationTime(const QDateTime &mtime)
{
    Q_D(FileCopyJob);
    d->m_modificationTime = mtime;
}

QUrl FileCopyJob::srcUrl() const
{
    return d_func()->m_src;
}

QUrl FileCopyJob::destUrl() const
{
    return d_func()->m_dest;
}

void FileCopyJobPrivate::startCopyJob()
{
    startCopyJob(m_src);
}

void FileCopyJobPrivate::startCopyJob(const QUrl &slave_url)
{
    Q_Q(FileCopyJob);
    //qDebug();
    KIO_ARGS << m_src << m_dest << m_permissions << (qint8)(m_flags & Overwrite);
    m_copyJob = new DirectCopyJob(slave_url, packedArgs);
    m_copyJob->setParentJob(q);
    if (m_modificationTime.isValid()) {
        m_copyJob->addMetaData(QStringLiteral("modified"), m_modificationTime.toString(Qt::ISODate));     // #55804
    }
    q->addSubjob(m_copyJob);
    connectSubjob(m_copyJob);
    q->connect(m_copyJob, SIGNAL(canResume(KIO::Job*,KIO::filesize_t)),
               SLOT(slotCanResume(KIO::Job*,KIO::filesize_t)));
}

void FileCopyJobPrivate::startRenameJob(const QUrl &slave_url)
{
    Q_Q(FileCopyJob);
    m_mustChmod = true;  // CMD_RENAME by itself doesn't change permissions
    KIO_ARGS << m_src << m_dest << (qint8)(m_flags & Overwrite);
    m_moveJob = SimpleJobPrivate::newJobNoUi(slave_url, CMD_RENAME, packedArgs);
    m_moveJob->setParentJob(q);
    if (m_modificationTime.isValid()) {
        m_moveJob->addMetaData(QStringLiteral("modified"), m_modificationTime.toString(Qt::ISODate));     // #55804
    }
    q->addSubjob(m_moveJob);
    connectSubjob(m_moveJob);
}

void FileCopyJobPrivate::connectSubjob(SimpleJob *job)
{
    Q_Q(FileCopyJob);
    q->connect(job, SIGNAL(totalSize(KJob*,qulonglong)),
               SLOT(slotTotalSize(KJob*,qulonglong)));

    q->connect(job, SIGNAL(processedSize(KJob*,qulonglong)),
               SLOT(slotProcessedSize(KJob*,qulonglong)));

    q->connect(job, SIGNAL(percent(KJob*,ulong)),
               SLOT(slotPercent(KJob*,ulong)));
    if (q->isSuspended()) {
        job->suspend();
    }
}

bool FileCopyJob::doSuspend()
{
    Q_D(FileCopyJob);
    if (d->m_moveJob) {
        d->m_moveJob->suspend();
    }

    if (d->m_copyJob) {
        d->m_copyJob->suspend();
    }

    if (d->m_getJob) {
        d->m_getJob->suspend();
    }

    if (d->m_putJob) {
        d->m_putJob->suspend();
    }

    Job::doSuspend();
    return true;
}

bool FileCopyJob::doResume()
{
    Q_D(FileCopyJob);
    if (d->m_moveJob) {
        d->m_moveJob->resume();
    }

    if (d->m_copyJob) {
        d->m_copyJob->resume();
    }

    if (d->m_getJob) {
        d->m_getJob->resume();
    }

    if (d->m_putJob) {
        d->m_putJob->resume();
    }

    Job::doResume();
    return true;
}

void FileCopyJobPrivate::slotProcessedSize(KJob *, qulonglong size)
{
    Q_Q(FileCopyJob);
    q->setProcessedAmount(KJob::Bytes, size);
}

void FileCopyJobPrivate::slotTotalSize(KJob *, qulonglong size)
{
    Q_Q(FileCopyJob);
    if (size != q->totalAmount(KJob::Bytes)) {
        q->setTotalAmount(KJob::Bytes, size);
    }
}

void FileCopyJobPrivate::slotPercent(KJob *, unsigned long pct)
{
    Q_Q(FileCopyJob);
    if (pct > q->percent()) {
        q->setPercent(pct);
    }
}

void FileCopyJobPrivate::startDataPump()
{
    Q_Q(FileCopyJob);
    //qDebug();

    m_canResume = false;
    m_resumeAnswerSent = false;
    m_getJob = nullptr; // for now
    m_putJob = put(m_dest, m_permissions, (m_flags | HideProgressInfo) /* no GUI */);
    m_putJob->setParentJob(q);
    //qDebug() << "m_putJob=" << m_putJob << "m_dest=" << m_dest;
    if (m_modificationTime.isValid()) {
        m_putJob->setModificationTime(m_modificationTime);
    }

    // The first thing the put job will tell us is whether we can
    // resume or not (this is always emitted)
    q->connect(m_putJob, SIGNAL(canResume(KIO::Job*,KIO::filesize_t)),
               SLOT(slotCanResume(KIO::Job*,KIO::filesize_t)));
    q->connect(m_putJob, SIGNAL(dataReq(KIO::Job*,QByteArray&)),
               SLOT(slotDataReq(KIO::Job*,QByteArray&)));
    q->addSubjob(m_putJob);
}

void FileCopyJobPrivate::slotCanResume(KIO::Job *job, KIO::filesize_t offset)
{
    Q_Q(FileCopyJob);
    if (job == m_putJob || job == m_copyJob) {
        //qDebug() << "'can resume' from PUT job. offset=" << KIO::number(offset);
        if (offset) {
            RenameDialog_Result res = R_RESUME;

            if (!KProtocolManager::autoResume() && !(m_flags & Overwrite) && m_uiDelegateExtension) {
                QString newPath;
                KIO::Job *job = (q->parentJob()) ? q->parentJob() : q;
                // Ask confirmation about resuming previous transfer
                res = m_uiDelegateExtension->askFileRename(
                          job, i18n("File Already Exists"),
                          m_src,
                          m_dest,
                          RenameDialog_Options(RenameDialog_Overwrite | RenameDialog_Resume | RenameDialog_NoRename), newPath,
                          m_sourceSize, offset);
            }

            if (res == R_OVERWRITE || (m_flags & Overwrite)) {
                offset = 0;
            } else if (res == R_CANCEL) {
                if (job == m_putJob) {
                    m_putJob->kill(FileCopyJob::Quietly);
                    q->removeSubjob(m_putJob);
                    m_putJob = nullptr;
                } else {
                    m_copyJob->kill(FileCopyJob::Quietly);
                    q->removeSubjob(m_copyJob);
                    m_copyJob = nullptr;
                }
                q->setError(ERR_USER_CANCELED);
                q->emitResult();
                return;
            }
        } else {
            m_resumeAnswerSent = true;    // No need for an answer
        }

        if (job == m_putJob) {
            m_getJob = KIO::get(m_src, NoReload, HideProgressInfo /* no GUI */);
            m_getJob->setParentJob(q);
            //qDebug() << "m_getJob=" << m_getJob << m_src;
            m_getJob->addMetaData(QStringLiteral("errorPage"), QStringLiteral("false"));
            m_getJob->addMetaData(QStringLiteral("AllowCompressedPage"), QStringLiteral("false"));
            // Set size in subjob. This helps if the slave doesn't emit totalSize.
            if (m_sourceSize != (KIO::filesize_t) - 1) {
                m_getJob->setTotalAmount(KJob::Bytes, m_sourceSize);
            }
            if (offset) {
                //qDebug() << "Setting metadata for resume to" << (unsigned long) offset;
                m_getJob->addMetaData(QStringLiteral("range-start"), KIO::number(offset));

                // Might or might not get emitted
                q->connect(m_getJob, SIGNAL(canResume(KIO::Job*,KIO::filesize_t)),
                           SLOT(slotCanResume(KIO::Job*,KIO::filesize_t)));
            }
            jobSlave(m_putJob)->setOffset(offset);

            m_putJob->d_func()->internalSuspend();
            q->addSubjob(m_getJob);
            connectSubjob(m_getJob);   // Progress info depends on get
            m_getJob->d_func()->internalResume(); // Order a beer

            q->connect(m_getJob, SIGNAL(data(KIO::Job*,QByteArray)),
                       SLOT(slotData(KIO::Job*,QByteArray)));
            q->connect(m_getJob, SIGNAL(mimetype(KIO::Job*,QString)),
                       SLOT(slotMimetype(KIO::Job*,QString)));
        } else { // copyjob
            jobSlave(m_copyJob)->sendResumeAnswer(offset != 0);
        }
    } else if (job == m_getJob) {
        // Cool, the get job said ok, we can resume
        m_canResume = true;
        //qDebug() << "'can resume' from the GET job -> we can resume";

        jobSlave(m_getJob)->setOffset(jobSlave(m_putJob)->offset());
    } else {
        qCWarning(KIO_CORE) << "unknown job=" << job << "m_getJob=" << m_getJob << "m_putJob=" << m_putJob;
    }
}

void FileCopyJobPrivate::slotData(KIO::Job *, const QByteArray &data)
{
    //qDebug() << "data size:" << data.size();
    Q_ASSERT(m_putJob);
    if (!m_putJob) {
        return;    // Don't crash
    }
    m_getJob->d_func()->internalSuspend();
    m_putJob->d_func()->internalResume(); // Drink the beer
    m_buffer += data;

    // On the first set of data incoming, we tell the "put" slave about our
    // decision about resuming
    if (!m_resumeAnswerSent) {
        m_resumeAnswerSent = true;
        //qDebug() << "(first time) -> send resume answer " << m_canResume;
        jobSlave(m_putJob)->sendResumeAnswer(m_canResume);
    }
}

void FileCopyJobPrivate::slotDataReq(KIO::Job *, QByteArray &data)
{
    Q_Q(FileCopyJob);
    //qDebug();
    if (!m_resumeAnswerSent && !m_getJob) {
        // This can't happen
        q->setError(ERR_INTERNAL);
        q->setErrorText(QStringLiteral("'Put' job did not send canResume or 'Get' job did not send data!"));
        m_putJob->kill(FileCopyJob::Quietly);
        q->removeSubjob(m_putJob);
        m_putJob = nullptr;
        q->emitResult();
        return;
    }
    if (m_getJob) {
        m_getJob->d_func()->internalResume(); // Order more beer
        m_putJob->d_func()->internalSuspend();
    }
    data = m_buffer;
    m_buffer = QByteArray();
}

void FileCopyJobPrivate::slotMimetype(KIO::Job *, const QString &type)
{
    Q_Q(FileCopyJob);
    emit q->mimetype(q, type);
}

void FileCopyJob::slotResult(KJob *job)
{
    Q_D(FileCopyJob);
    //qDebug() << "this=" << this << "job=" << job;
    removeSubjob(job);
    // Did job have an error ?
    if (job->error()) {
        if ((job == d->m_moveJob) && (job->error() == ERR_UNSUPPORTED_ACTION)) {
            d->m_moveJob = nullptr;
            d->startBestCopyMethod();
            return;
        } else if ((job == d->m_copyJob) && (job->error() == ERR_UNSUPPORTED_ACTION)) {
            d->m_copyJob = nullptr;
            d->startDataPump();
            return;
        } else if (job == d->m_getJob) {
            d->m_getJob = nullptr;
            if (d->m_putJob) {
                d->m_putJob->kill(Quietly);
                removeSubjob(d->m_putJob);
            }
        } else if (job == d->m_putJob) {
            d->m_putJob = nullptr;
            if (d->m_getJob) {
                d->m_getJob->kill(Quietly);
                removeSubjob(d->m_getJob);
            }
        }
        setError(job->error());
        setErrorText(job->errorText());
        emitResult();
        return;
    }

    if (d->m_mustChmod) {
        // If d->m_permissions == -1, keep the default permissions
        if (d->m_permissions != -1) {
            d->m_chmodJob = chmod(d->m_dest, d->m_permissions);
            addSubjob(d->m_chmodJob);
        }
        d->m_mustChmod = false;
    }

    if (job == d->m_moveJob) {
        d->m_moveJob = nullptr; // Finished
    }

    if (job == d->m_copyJob) {
        d->m_copyJob = nullptr;
        if (d->m_move) {
            d->m_delJob = file_delete(d->m_src, HideProgressInfo/*no GUI*/);   // Delete source
            addSubjob(d->m_delJob);
        }
    }

    if (job == d->m_getJob) {
        //qDebug() << "m_getJob finished";
        d->m_getJob = nullptr; // No action required
        if (d->m_putJob) {
            d->m_putJob->d_func()->internalResume();
        }
    }

    if (job == d->m_putJob) {
        //qDebug() << "m_putJob finished";
        d->m_putJob = nullptr;
        if (d->m_getJob) {
            // The get job is still running, probably after emitting data(QByteArray())
            // and before we receive its finished().
            d->m_getJob->d_func()->internalResume();
        }
        if (d->m_move) {
            d->m_delJob = file_delete(d->m_src, HideProgressInfo/*no GUI*/);   // Delete source
            addSubjob(d->m_delJob);
        }
    }

    if (job == d->m_delJob) {
        d->m_delJob = nullptr; // Finished
    }

    if (job == d->m_chmodJob) {
        d->m_chmodJob = nullptr; // Finished
    }

    if (!hasSubjobs()) {
        emitResult();
    }
}

FileCopyJob *KIO::file_copy(const QUrl &src, const QUrl &dest, int permissions,
                            JobFlags flags)
{
    return FileCopyJobPrivate::newJob(src, dest, permissions, false, flags);
}

FileCopyJob *KIO::file_move(const QUrl &src, const QUrl &dest, int permissions,
                            JobFlags flags)
{
    FileCopyJob *job = FileCopyJobPrivate::newJob(src, dest, permissions, true, flags);
    if (job->uiDelegateExtension()) {
        job->uiDelegateExtension()->createClipboardUpdater(job, JobUiDelegateExtension::UpdateContent);
    }
    return job;
}

#include "moc_filecopyjob.cpp"
