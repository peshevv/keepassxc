/*
 *  Copyright (C) 2012 Felix Geyer <debfx@fobos.de>
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

#include "EntryAttachments.h"

#include "core/Global.h"
#include "crypto/Random.h"

#include <QDesktopServices>
#include <QDir>
#include <QProcessEnvironment>
#include <QSet>
#include <QTemporaryFile>
#include <QStringList>
#include <QUrl>

EntryAttachments::EntryAttachments(QObject* parent)
    : ModifiableObject(parent)
{
}

EntryAttachments::~EntryAttachments()
{
    clear();
}

QList<QString> EntryAttachments::keys() const
{
    return m_attachments.keys();
}

bool EntryAttachments::hasKey(const QString& key) const
{
    return m_attachments.contains(key);
}

QSet<QByteArray> EntryAttachments::values() const
{
    return asConst(m_attachments).values().toSet();
}

QByteArray EntryAttachments::value(const QString& key) const
{
    return m_attachments.value(key);
}

void EntryAttachments::set(const QString& key, const QByteArray& value)
{
    bool shouldEmitModified = false;
    bool addAttachment = !m_attachments.contains(key);

    if (addAttachment) {
        emit aboutToBeAdded(key);
    }

    if (addAttachment || m_attachments.value(key) != value) {
        m_attachments.insert(key, value);
        shouldEmitModified = true;
    }

    if (addAttachment) {
        emit added(key);
    } else {
        emit keyModified(key);
    }

    if (shouldEmitModified) {
        emitModified();
    }
}

void EntryAttachments::remove(const QString& key)
{
    if (!m_attachments.contains(key)) {
        Q_ASSERT_X(false, "EntryAttachments::remove", qPrintable(QString("Can't find attachment for key %1").arg(key)));
        return;
    }

    emit aboutToBeRemoved(key);

    m_attachments.remove(key);

    emit removed(key);
    emitModified();
}

void EntryAttachments::remove(const QStringList& keys)
{
    if (keys.isEmpty()) {
        return;
    }

    bool isModified = false;
    for (const QString& key : keys) {
        if (!m_attachments.contains(key)) {
            Q_ASSERT_X(
                false, "EntryAttachments::remove", qPrintable(QString("Can't find attachment for key %1").arg(key)));
            continue;
        }

        isModified = true;
        emit aboutToBeRemoved(key);
        m_attachments.remove(key);
        emit removed(key);
    }

    if (isModified) {
        emitModified();
    }
}

void EntryAttachments::rename(const QString& key, const QString& newKey)
{
    const QByteArray val = value(key);
    remove(key);
    set(newKey, val);
}

bool EntryAttachments::isEmpty() const
{
    return m_attachments.isEmpty();
}

void EntryAttachments::clear()
{
    if (m_attachments.isEmpty()) {
        return;
    }

    emit aboutToBeReset();

    m_attachments.clear();

    // Overwrite all open attachment files with random data and then remove them
    for (auto& path : m_attachmentFileWatcher.files()) {
        QFile f(path);
        if (f.open(QFile::ReadWrite)) {
            qint64 s = f.size();
            for (qint64 i = 0; i < (s / 128 + 1); ++i) {
                f.write(randomGen()->randomArray(128));
            }
        }
        f.close();
        f.remove();
        m_attachmentFileWatcher.removePath(path);
    }

    emit reset();
    emitModified();
}

void EntryAttachments::copyDataFrom(const EntryAttachments* other)
{
    if (*this != *other) {
        emit aboutToBeReset();

        m_attachments = other->m_attachments;

        emit reset();
        emitModified();
    }
}

bool EntryAttachments::operator==(const EntryAttachments& other) const
{
    return m_attachments == other.m_attachments;
}

bool EntryAttachments::operator!=(const EntryAttachments& other) const
{
    return m_attachments != other.m_attachments;
}

int EntryAttachments::attachmentsSize() const
{
    int size = 0;
    for (auto it = m_attachments.constBegin(); it != m_attachments.constEnd(); ++it) {
        size += it.key().toUtf8().size() + it.value().size();
    }
    return size;
}

bool EntryAttachments::openAttachment(const QString& key, QString* errorMessage)
{

    const QByteArray attachmentData = value(key);
    auto ext = key.contains(".") ? key.split(".").last() : "";

#ifdef KEEPASSXC_DIST_SNAP
    const QString tmpFileTemplate =
        QString("%1/XXXXXXXXXXXX.%2").arg(QProcessEnvironment::systemEnvironment().value("SNAP_USER_DATA"), ext);
#else
    const QString tmpFileTemplate = QDir::temp().absoluteFilePath(QString("XXXXXXXXXXXX.").append(ext));
#endif

    QTemporaryFile tmpFile(tmpFileTemplate);

    const bool saveOk = tmpFile.open() && tmpFile.write(attachmentData) == attachmentData.size() && tmpFile.flush();
    if (!saveOk && errorMessage) {
        *errorMessage = tr("%1 - %2").arg(key, tmpFile.errorString());
        return false;
    }

    m_attachmentFileWatcher.addPath(tmpFile.fileName());
    tmpFile.close();
    tmpFile.setAutoRemove(false);
    const bool openOk = QDesktopServices::openUrl(QUrl::fromLocalFile(tmpFile.fileName()));
    if (!openOk && errorMessage) {
        *errorMessage = tr("Cannot open file \"%1\"").arg(key);
        return false;
    }

    return true;
}
