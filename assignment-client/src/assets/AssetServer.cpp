//
//  AssetServer.cpp
//  assignment-client/src/assets
//
//  Created by Ryan Huffman on 2015/07/21
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//


#include "AssetServer.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include "NetworkLogging.h"
#include "NodeType.h"
#include "SendAssetTask.h"
#include "UploadAssetTask.h"
#include <ServerPathUtils.h>

const QString ASSET_SERVER_LOGGING_TARGET_NAME = "asset-server";

AssetServer::AssetServer(ReceivedMessage& message) :
    ThreadedAssignment(message),
    _taskPool(this)
{

    // Most of the work will be I/O bound, reading from disk and constructing packet objects,
    // so the ideal is greater than the number of cores on the system.
    static const int TASK_POOL_THREAD_COUNT = 50;
    _taskPool.setMaxThreadCount(TASK_POOL_THREAD_COUNT);

    auto& packetReceiver = DependencyManager::get<NodeList>()->getPacketReceiver();
    packetReceiver.registerListener(PacketType::AssetGet, this, "handleAssetGet");
    packetReceiver.registerListener(PacketType::AssetGetInfo, this, "handleAssetGetInfo");
    packetReceiver.registerListener(PacketType::AssetUpload, this, "handleAssetUpload");
    packetReceiver.registerListener(PacketType::AssetMappingOperation, this, "handleAssetMappingOperation");
}

void AssetServer::run() {

    qDebug() << "Waiting for connection to domain to request settings from domain-server.";

    // wait until we have the domain-server settings, otherwise we bail
    DomainHandler& domainHandler = DependencyManager::get<NodeList>()->getDomainHandler();
    connect(&domainHandler, &DomainHandler::settingsReceived, this, &AssetServer::completeSetup);
    connect(&domainHandler, &DomainHandler::settingsReceiveFail, this, &AssetServer::domainSettingsRequestFailed);

    ThreadedAssignment::commonInit(ASSET_SERVER_LOGGING_TARGET_NAME, NodeType::AssetServer);
}

void AssetServer::completeSetup() {
    auto nodeList = DependencyManager::get<NodeList>();

    auto& domainHandler = nodeList->getDomainHandler();
    const QJsonObject& settingsObject = domainHandler.getSettingsObject();

    static const QString ASSET_SERVER_SETTINGS_KEY = "asset_server";

    if (!settingsObject.contains(ASSET_SERVER_SETTINGS_KEY)) {
        qCritical() << "Received settings from the domain-server with no asset-server section. Stopping assignment.";
        setFinished(true);
        return;
    }

    auto assetServerObject = settingsObject[ASSET_SERVER_SETTINGS_KEY].toObject();

    // get the path to the asset folder from the domain server settings
    static const QString ASSETS_PATH_OPTION = "assets_path";
    auto assetsJSONValue = assetServerObject[ASSETS_PATH_OPTION];

    if (!assetsJSONValue.isString()) {
        qCritical() << "Received an assets path from the domain-server that could not be parsed. Stopping assignment.";
        setFinished(true);
        return;
    }

    auto assetsPathString = assetsJSONValue.toString();
    QDir assetsPath { assetsPathString };
    QString absoluteFilePath = assetsPath.absolutePath();

    if (assetsPath.isRelative()) {
        // if the domain settings passed us a relative path, make an absolute path that is relative to the
        // default data directory
        absoluteFilePath = ServerPathUtils::getDataFilePath("assets/" + assetsPathString);
    }

    _resourcesDirectory = QDir(absoluteFilePath);

    qDebug() << "Creating resources directory";
    _resourcesDirectory.mkpath(".");

    bool noExistingAssets = !_resourcesDirectory.exists() || _resourcesDirectory.entryList(QDir::Files).size() == 0;

    if (noExistingAssets) {
        qDebug() << "Asset resources directory empty, searching for existing asset resources to migrate";
        QString oldDataDirectory = QCoreApplication::applicationDirPath();

        const QString OLD_RESOURCES_PATH = "assets";

        auto oldResourcesDirectory = QDir(oldDataDirectory).filePath("resources/" + OLD_RESOURCES_PATH);


        if (QDir(oldResourcesDirectory).exists()) {
            qDebug() << "Existing assets found in " << oldResourcesDirectory << ", copying to " << _resourcesDirectory;


            QDir resourcesParentDirectory = _resourcesDirectory.filePath("..");
            if (!resourcesParentDirectory.exists()) {
                qDebug() << "Creating data directory " << resourcesParentDirectory.absolutePath();
                resourcesParentDirectory.mkpath(".");
            }

            auto files = QDir(oldResourcesDirectory).entryList(QDir::Files);

            for (auto& file : files) {
                auto from = oldResourcesDirectory + QDir::separator() + file;
                auto to = _resourcesDirectory.absoluteFilePath(file);
                qDebug() << "\tCopying from " << from << " to " << to;
                QFile::copy(from, to);
            }
        }
    }
    
    qInfo() << "Serving files from: " << _resourcesDirectory.path();

    // Check the asset directory to output some information about what we have
    auto files = _resourcesDirectory.entryList(QDir::Files);

    QRegExp hashFileRegex { "^[a-f0-9]{" + QString::number(SHA256_HASH_HEX_LENGTH) + "}$" };
    auto hashedFiles = files.filter(hashFileRegex);

    qInfo() << "There are" << hashedFiles.size() << "asset files in the asset directory.";

    performMappingMigration();

    nodeList->addNodeTypeToInterestSet(NodeType::Agent);
}

void AssetServer::performMappingMigration() {
    QRegExp hashFileRegex { "^[a-f0-9]{" + QString::number(SHA256_HASH_HEX_LENGTH) + "}(\\.[\\w]+)+$" };

    auto files = _resourcesDirectory.entryInfoList(QDir::Files);

    for (const auto& fileInfo : files) {
        if (hashFileRegex.exactMatch(fileInfo.fileName())) {
            // we have a pre-mapping file that we should migrate to the new mapping system
            qDebug() << "Migrating pre-mapping file" << fileInfo.fileName();

            // rename the file to the same name with no extension
            QFile oldFile { fileInfo.absoluteFilePath() };

            auto oldAbsolutePath = fileInfo.absoluteFilePath();
            auto oldFilename = fileInfo.fileName();
            auto fullExtension = oldFilename.mid(oldFilename.indexOf('.'));

            qDebug() << "Moving" << oldAbsolutePath << "to" << oldAbsolutePath.replace(fullExtension, "");

            bool renamed = oldFile.rename(oldAbsolutePath.replace(fullExtension, ""));
            if (!renamed) {
                qWarning() << "Could not migrate pre-mapping file" << fileInfo.fileName();
            } else {
                qDebug() << "Renamed pre-mapping file" << fileInfo.fileName();

                // add a new mapping with the old extension and a truncated version of the hash
            }
        }
    }
}

void AssetServer::handleAssetMappingOperation(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    MessageID messageID;
    message->readPrimitive(&messageID);

    AssetMappingOperationType operationType;
    message->readPrimitive(&operationType);

    auto replyPacket = NLPacket::create(PacketType::AssetMappingOperationReply);
    replyPacket->writePrimitive(messageID);

    switch (operationType) {
        case AssetMappingOperationType::Get: {
            QString assetPath = message->readString();

            auto it = _fileMapping.find(assetPath);
            if (it != _fileMapping.end()) {
                auto assetHash = it->second;
                qDebug() << "Found mapping for: " << assetPath << "=>" << assetHash;
                replyPacket->writePrimitive(AssetServerError::NoError);
                //replyPacket->write(assetHash.toLatin1().toHex());
                replyPacket->writeString(assetHash.toLatin1());
            }
            else {
                qDebug() << "Mapping not found for: " << assetPath;
                replyPacket->writePrimitive(AssetServerError::AssetNotFound);
            }
            break;
        }
        case AssetMappingOperationType::Set: {
            QString assetPath = message->readString();
            //auto assetHash = message->read(SHA256_HASH_LENGTH);
            auto assetHash = message->readString();
            _fileMapping[assetPath] = assetHash;

            replyPacket->writePrimitive(AssetServerError::NoError);
            break;
        }
        case AssetMappingOperationType::Delete: {
            QString assetPath = message->readString();
            bool removed = _fileMapping.erase(assetPath) > 0;

            replyPacket->writePrimitive(AssetServerError::NoError);
            break;
        }
    }

    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->sendPacket(std::move(replyPacket), *senderNode);
}

void AssetServer::handleAssetGetInfo(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    QByteArray assetHash;
    MessageID messageID;

    if (message->getSize() < qint64(SHA256_HASH_LENGTH + sizeof(messageID))) {
        qDebug() << "ERROR bad file request";
        return;
    }

    message->readPrimitive(&messageID);
    assetHash = message->readWithoutCopy(SHA256_HASH_LENGTH);

    auto replyPacket = NLPacket::create(PacketType::AssetGetInfoReply);

    QByteArray hexHash = assetHash.toHex();

    replyPacket->writePrimitive(messageID);
    replyPacket->write(assetHash);

    QString fileName = QString(hexHash);
    QFileInfo fileInfo { _resourcesDirectory.filePath(fileName) };

    if (fileInfo.exists() && fileInfo.isReadable()) {
        qDebug() << "Opening file: " << fileInfo.filePath();
        replyPacket->writePrimitive(AssetServerError::NoError);
        replyPacket->writePrimitive(fileInfo.size());
    } else {
        qDebug() << "Asset not found: " << QString(hexHash);
        replyPacket->writePrimitive(AssetServerError::AssetNotFound);
    }

    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->sendPacket(std::move(replyPacket), *senderNode);
}

void AssetServer::handleAssetGet(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {

    auto minSize = qint64(sizeof(MessageID) + SHA256_HASH_LENGTH + sizeof(DataOffset) + sizeof(DataOffset));
    
    if (message->getSize() < minSize) {
        qDebug() << "ERROR bad file request";
        return;
    }

    // Queue task
    auto task = new SendAssetTask(message, senderNode, _resourcesDirectory);
    _taskPool.start(task);
}

void AssetServer::handleAssetUpload(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    
    if (senderNode->getCanRez()) {
        qDebug() << "Starting an UploadAssetTask for upload from" << uuidStringWithoutCurlyBraces(senderNode->getUUID());
        
        auto task = new UploadAssetTask(message, senderNode, _resourcesDirectory);
        _taskPool.start(task);
    } else {
        // this is a node the domain told us is not allowed to rez entities
        // for now this also means it isn't allowed to add assets
        // so return a packet with error that indicates that
        
        auto permissionErrorPacket = NLPacket::create(PacketType::AssetUploadReply, sizeof(MessageID) + sizeof(AssetServerError));
        
        MessageID messageID;
        message->readPrimitive(&messageID);
        
        // write the message ID and a permission denied error
        permissionErrorPacket->writePrimitive(messageID);
        permissionErrorPacket->writePrimitive(AssetServerError::PermissionDenied);
        
        // send off the packet
        auto nodeList = DependencyManager::get<NodeList>();
        nodeList->sendPacket(std::move(permissionErrorPacket), *senderNode);
    }
}

void AssetServer::sendStatsPacket() {
    QJsonObject serverStats;
    
    auto stats = DependencyManager::get<NodeList>()->sampleStatsForAllConnections();
    
    for (const auto& stat : stats) {
        QJsonObject nodeStats;
        auto endTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(stat.second.endTime);
        QDateTime date = QDateTime::fromMSecsSinceEpoch(endTimeMs.count());
        
        static const float USEC_PER_SEC = 1000000.0f;
        static const float MEGABITS_PER_BYTE = 8.0f / 1000000.0f; // Bytes => Mbits
        float elapsed = (float)(stat.second.endTime - stat.second.startTime).count() / USEC_PER_SEC; // sec
        float megabitsPerSecPerByte = MEGABITS_PER_BYTE / elapsed; // Bytes => Mb/s
        
        QJsonObject connectionStats;
        connectionStats["1. Last Heard"] = date.toString();
        connectionStats["2. Est. Max (P/s)"] = stat.second.estimatedBandwith;
        connectionStats["3. RTT (ms)"] = stat.second.rtt;
        connectionStats["4. CW (P)"] = stat.second.congestionWindowSize;
        connectionStats["5. Period (us)"] = stat.second.packetSendPeriod;
        connectionStats["6. Up (Mb/s)"] = stat.second.sentBytes * megabitsPerSecPerByte;
        connectionStats["7. Down (Mb/s)"] = stat.second.receivedBytes * megabitsPerSecPerByte;
        nodeStats["Connection Stats"] = connectionStats;
        
        using Events = udt::ConnectionStats::Stats::Event;
        const auto& events = stat.second.events;
        
        QJsonObject upstreamStats;
        upstreamStats["1. Sent (P/s)"] = stat.second.sendRate;
        upstreamStats["2. Sent Packets"] = stat.second.sentPackets;
        upstreamStats["3. Recvd ACK"] = events[Events::ReceivedACK];
        upstreamStats["4. Procd ACK"] = events[Events::ProcessedACK];
        upstreamStats["5. Recvd LACK"] = events[Events::ReceivedLightACK];
        upstreamStats["6. Recvd NAK"] = events[Events::ReceivedNAK];
        upstreamStats["7. Recvd TNAK"] = events[Events::ReceivedTimeoutNAK];
        upstreamStats["8. Sent ACK2"] = events[Events::SentACK2];
        upstreamStats["9. Retransmitted"] = events[Events::Retransmission];
        nodeStats["Upstream Stats"] = upstreamStats;
        
        QJsonObject downstreamStats;
        downstreamStats["1. Recvd (P/s)"] = stat.second.receiveRate;
        downstreamStats["2. Recvd Packets"] = stat.second.receivedPackets;
        downstreamStats["3. Sent ACK"] = events[Events::SentACK];
        downstreamStats["4. Sent LACK"] = events[Events::SentLightACK];
        downstreamStats["5. Sent NAK"] = events[Events::SentNAK];
        downstreamStats["6. Sent TNAK"] = events[Events::SentTimeoutNAK];
        downstreamStats["7. Recvd ACK2"] = events[Events::ReceivedACK2];
        downstreamStats["8. Duplicates"] = events[Events::Duplicate];
        nodeStats["Downstream Stats"] = downstreamStats;
        
        QString uuid;
        auto nodelist = DependencyManager::get<NodeList>();
        if (stat.first == nodelist->getDomainHandler().getSockAddr()) {
            uuid = uuidStringWithoutCurlyBraces(nodelist->getDomainHandler().getUUID());
            nodeStats[USERNAME_UUID_REPLACEMENT_STATS_KEY] = "DomainServer";
        } else {
            auto node = nodelist->findNodeWithAddr(stat.first);
            uuid = uuidStringWithoutCurlyBraces(node ? node->getUUID() : QUuid());
            nodeStats[USERNAME_UUID_REPLACEMENT_STATS_KEY] = uuid;
        }
        
        serverStats[uuid] = nodeStats;
    }
    
    // send off the stats packets
    ThreadedAssignment::addPacketStatsAndSendStatsPacket(serverStats);
}

void AssetServer::loadMappingFromFile() {
}

void AssetServer::writeMappingToFile() {
}

AssetHash AssetServer::getMapping(AssetPath path) {
    return _fileMapping[path];
}

void AssetServer::setMapping(AssetPath path, AssetHash hash) {
    _fileMapping[path] = hash;
}

bool AssetServer::deleteMapping(AssetPath path) {
    return _fileMapping.erase(path) > 0;
}
