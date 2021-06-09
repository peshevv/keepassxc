/*
 *  Copyright (C) 2020 KeePassXC Team <team@keepassxc.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "FileWatcher.h"

#include "core/AsyncTask.h"

#include <QCryptographicHash>
#include <QtGlobal>

#ifdef Q_OS_LINUX
#include <sys/vfs.h>
#endif

class FileWatcherPrivate : public QObject
{
    Q_OBJECT

public:
    explicit FileWatcherPrivate(QObject* parent = nullptr)
        : QObject(parent)
    {
        connect(&m_fileChecksumTimer, SIGNAL(timeout()), SLOT(checkFileChanged()));
        connect(&m_fileChangeDelayTimer, &QTimer::timeout, this, [this] { emit fileChanged(m_filePath); });
        m_fileChangeDelayTimer.setSingleShot(true);
        m_fileIgnoreDelayTimer.setSingleShot(true);
    }

    virtual ~FileWatcherPrivate()
    {
        stop();
    }

    void start(const QString& path, int checksumIntervalSeconds = 0, int checksumSizeKibibytes = -1)
    {
        stop();

#if defined(Q_OS_LINUX)
        struct statfs statfsBuf;
    bool forcePolling = false;
    const auto NFS_SUPER_MAGIC = 0x6969;

    if (!statfs(filePath.toLocal8Bit().constData(), &statfsBuf)) {
        forcePolling = (statfsBuf.f_type == NFS_SUPER_MAGIC);
    } else {
        // if we can't get the fs type let's fall back to polling
        forcePolling = true;
    }
    auto objectName = forcePolling ? QLatin1String("_qt_autotest_force_engine_poller") : QLatin1String("");
    m_fileWatcher.setObjectName(objectName);
#endif

        m_filePath = path;

        // Handle file checksum
        m_fileChecksumSizeBytes = checksumSizeKibibytes * 1024;
        m_fileChecksum = calculateChecksum();
        if (checksumIntervalSeconds > 0) {
            m_fileChecksumTimer.start(checksumIntervalSeconds * 1000);
        }

        m_ignoreFileChange = false;
    }

    void stop()
    {
        m_filePath.clear();
        m_fileChecksum.clear();
        m_fileChecksumTimer.stop();
        m_fileChangeDelayTimer.stop();
    }

    bool hasSameFileChecksum()
    {
        return calculateChecksum() == m_fileChecksum;
    }

signals:
    void fileChanged(const QString& path);

public slots:
    void pause()
    {
        m_ignoreFileChange = true;
        m_fileChangeDelayTimer.stop();
    }

    void resume()
    {
        m_ignoreFileChange = false;
        // Add a short delay to start in the next event loop
        if (!m_fileIgnoreDelayTimer.isActive()) {
            m_fileIgnoreDelayTimer.start(0);
        }
    }

    void checkFileChanged()
    {
        if (shouldIgnoreChanges()) {
            return;
        }

        // Prevent reentrance
        m_ignoreFileChange = true;

        AsyncTask::runThenCallback([=] { return calculateChecksum(); },
                                   this,
                                   [=](QByteArray checksum) {
                                       if (checksum != m_fileChecksum) {
                                           m_fileChecksum = checksum;
                                           m_fileChangeDelayTimer.start(0);
                                       }

                                       m_ignoreFileChange = false;
                                   });
    }

private:
    QByteArray calculateChecksum() const
    {
        QFile file(m_filePath);
        if (file.open(QFile::ReadOnly)) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (m_fileChecksumSizeBytes > 0) {
                hash.addData(file.read(m_fileChecksumSizeBytes));
            } else {
                hash.addData(&file);
            }
            return hash.result();
        }
        // If we fail to open the file return the last known checksum, this
        // prevents unnecessary merge requests on intermittent network shares
        return m_fileChecksum;
    }

    bool shouldIgnoreChanges() const
    {
        return m_filePath.isEmpty() || m_ignoreFileChange || m_fileIgnoreDelayTimer.isActive()
            || m_fileChangeDelayTimer.isActive();
    }

    QString m_filePath;
    QByteArray m_fileChecksum;
    QTimer m_fileChangeDelayTimer;
    QTimer m_fileIgnoreDelayTimer;
    QTimer m_fileChecksumTimer;
    int m_fileChecksumSizeBytes = -1;
    bool m_ignoreFileChange = false;
};


FileWatcher::FileWatcher(QObject* parent)
    : QObject(parent)
{
    connect(&m_fileWatcher, SIGNAL(fileChanged(QString)), SLOT(checkFileChanged(QString)));
}

FileWatcher::~FileWatcher()
{
    removeAllPaths();
}

void FileWatcher::addPath(const QString& path, int checksumIntervalSeconds, int checksumSizeKibibytes)
{
    if (!m_watches.contains(path)) {
        m_fileWatcher.addPath(path);
        auto w = QSharedPointer<FileWatcherPrivate>::create();
        m_watches.insert(path, w);
        connect(w.data(), SIGNAL(fileChanged(QString)), SIGNAL(fileChanged(QString)));
    }
    m_watches.value(path)->start(path, checksumIntervalSeconds, checksumSizeKibibytes);
}

void FileWatcher::removeAllPaths()
{
    for (auto& path : m_watches.keys()) {
        removePath(path);
    }
}

void FileWatcher::removePath(const QString& path)
{
    if (m_watches.contains(path)) {
        m_fileWatcher.removePath(path);
        m_watches.remove(path);
    }
}

void FileWatcher::checkFileChanged(const QString& path)
{
    auto it = m_watches.find(path);
    if (it != m_watches.end()) {
        (*it)->checkFileChanged();
    }
}

bool FileWatcher::hasSameFileChecksum(const QString& path)
{
    auto it = m_watches.find(path);
    if (it != m_watches.end()) {
        return (*it)->hasSameFileChecksum();
    }
    return false;
}

void FileWatcher::pause()
{
    for (auto& w : m_watches) {
        w->pause();
    }
}

void FileWatcher::resume()
{
    for (auto& w : m_watches) {
        w->resume();
    }
}

#include "FileWatcher.moc"
