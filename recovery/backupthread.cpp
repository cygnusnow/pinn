#include "backupthread.h"
#include "config.h"
#include "json.h"
#include "util.h"
#include "mbr.h"
#include "partitioninfo.h"
#include "osinfo.h"
#include "mydebug.h"

#include <QDir>
#include <QFile>
#include <QDebug>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTime>
#include <QVariantList>
#include <QVariantMap>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/ioctl.h>


BackupThread::BackupThread(QObject *parent, QString local) :
    QThread(parent),  _local(local), _extraSpacePerPartition(0), _part(5)
{
    TRACE
    /* local is "/tmp/media/sd*" or "/mnt" (in future) */
}

void BackupThread::addImage(const QVariantMap &entry)
{
    TRACE
    _images.append(entry);
}

void BackupThread::run()
{
    TRACE
    /* Calculate space requirements */
    quint64 totalDownloadSize = 0;
    foreach (QVariantMap entry, _images)
    {
        quint64 downloadSize = entry.value("backupsize").toULongLong();
        totalDownloadSize += downloadSize;
    }

    emit parsedImagesize(totalDownloadSize);

    /* Process each image */
    int errors=0;
    foreach (QVariantMap entry, _images)
    {
        // for each partition
        if (!processImage(entry))
            errors++;
    }

    emit statusUpdate(tr("Finish writing (sync)"));
    sync();
    emit completed(errors);
}

#define copyentry(key1,key2) \
    if (entry.contains(key2)) \
        ventry[key1] = entry.value(key2)

bool BackupThread::processImage(const QVariantMap & entry)
{
    TRACE
    QString backupFolder = entry.value("backupFolder").toString();
    QString os_name = entry.value("name").toString();
    //Read the os.json and partitions.json files into pInfo
    OsInfo * pOsInfo = new OsInfo(backupFolder, "", NULL);
    QList<PartitionInfo *> *partitions = pOsInfo->partitions();
    QVariantList partdevices = entry.value("partitions").toList();

    QDir dir;
    if (!dir.exists("/tmp/src"))
        dir.mkdir("/tmp/src");

    //partitions.json
    QVariantList tarballsizes = entry.value("partsizes").toList(); //in MB
    QVariantMap backupmap = Json::loadFromFile(backupFolder+"/partitions.json").toMap();
    if (backupmap.contains("partitions"))
    {
        QVariantList backuplist = backupmap.value("partitions").toList();
        for (int i=0; i<backuplist.count(); i++)
        {
            //Add "supports_backup" to prevent unsupported OSes from being backed up.
            //what if partclone?
            QString dev = partdevices[i].toString();
            QVariantMap pmap = backuplist[i].toMap();
            pmap.remove("tarball");
            //Check for emptyfs. Is it still empty? If not, remove attribute.
            pmap.remove("empty_fs");
            //Check for fstype=="raw" ->image file.
            if (pmap.value("filesystem_type").toString() =="raw")
            {
                int errorcode;

                QString mounttype = readexec(false, "sh -c \"blkid -o value -s TYPE "+dev+"\"", errorcode).trimmed();
                if (mounttype == "btrfs")
                {
                    qDebug() << tr("Cannot backup ")+entry.value("name").toString()+tr(" :BTRFS file format");
                    return(false);
                }
                //If it was raw or empty & change to fat or ext4 tar file, change partition parameters.
                pmap["filesystem_type"] = mounttype;
                if (mounttype.left(3)=="ext")
                    pmap["mkfs_options"] = "-O ^huge_file";

            }

            //WinIoT support
            // if ntfs and want_maximised=false
            if ((pmap.value("filesystem_type").toString() =="ntfs") && (pmap.value("want_maximised").toString() =="false"))
            {
                //convert to raw image type
                pmap["filesystem_type"] = "raw";
                //use dd to backup
            }
            else
            {
                pmap["uncompressed_tarball_size"] = tarballsizes[i].toULongLong();
                if (i==0)
                    pmap["partition_size_nominal"] = tarballsizes[i].toULongLong()+100; //extra 100MB for boot
                else
                    pmap["partition_size_nominal"] = tarballsizes[i].toULongLong()+500; //extra 500MB for other partitions
            }
            backuplist[i] = pmap;
        }
        backupmap["partitions"] = backuplist;
    }

    emit statusUpdate(tr("%1: Updating partitions.json").arg(os_name));
    Json::saveToFile(backupFolder+"/partitions.json", backupmap);

    qint64 downloadSize=0L;

    //Re-read the modified partitions.json file
    delete pOsInfo;
    pOsInfo = new OsInfo(backupFolder, "", NULL);
    partitions = pOsInfo->partitions();

    int i=0;
    foreach (PartitionInfo * pPart, *partitions)
    {
        QString label = pPart->label();
        QString part = partdevices[i].toString();
        QByteArray fstype   = pPart->fsType();

        QString dev = getDevice(part);

        //   Mount it
        QProcess::execute("mount -o ro "+dev+" /tmp/src");
        emit newDrive(dev);
        QString cmd;

        if (fstype=="raw")
        {
            //   dd/gzip image
            emit statusUpdate(tr("%1: Writing image (%2)").arg(os_name, QString(label)));
            cmd = "sh -c \"dd if=" +dev+ " obs=4M | gzip > "+ backupFolder+"/"+label+".img.gz\"";
        }
        else
        {
            //   tar gzip
            emit statusUpdate(tr("%1: Archiving (%2)").arg(os_name, QString(label)));
            cmd = "sh -c \"cd /tmp/src; tar -c . | gzip > "+ backupFolder+"/"+label+".tar.gz\"";
        }

        qDebug()<<cmd;
        if (QProcess::execute(cmd))
        {
            qDebug() << tr("Error writing ")+entry.value("name").toString()+tr(": Disk full?");
            delete pOsInfo;
            return(false);
        }

        QString filename(backupFolder+"/"+label);
        if (fstype=="raw")
            filename += ".img.gz";
        else
            filename += ".tar.gz";
        QFileInfo fi(filename);
        downloadSize += fi.size();
        //   UnMount it
        QProcess::execute("umount /tmp/src");
        i++;
    }

    dir.rmdir("/tmp/src");

    //Update JSON files
    //os.json
    QVariantMap ventry = Json::loadFromFile(backupFolder+"/os.json").toMap();
    copyentry("name","name");
    //  name = copied from installed_os.json (to include nickname etc. + backup date
    copyentry("name", "backupName");
    copyentry("description","description");
    copyentry("group","group");
    copyentry("password","password");
    copyentry("release_date","release_date");
    copyentry("username","username");
    //  download_size = sum of tar.gz files sizes in bytes?
    ventry["download_size"] = downloadSize;
    // no icon entry to use default
    ventry.remove("icon");
    emit statusUpdate(tr("%1: Updating os.json").arg(os_name));
    Json::saveToFile(backupFolder+"/os.json", ventry);
    sync();

    emit newImage(QString(backupFolder+"/os.json"));

    delete pOsInfo;

    return(true);
}


