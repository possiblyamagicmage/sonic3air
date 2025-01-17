/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2021 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen_netcore/pch.h"
#include "oxygen_netcore/network/NetConnection.h"
#include "oxygen_netcore/network/ConnectionListener.h"
#include "oxygen_netcore/network/ConnectionManager.h"
#include "oxygen_netcore/network/LowLevelPackets.h"
#include "oxygen_netcore/network/HighLevelPacketBase.h"
#include "oxygen_netcore/network/RequestBase.h"


uint64 NetConnection::buildSenderKey(const SocketAddress& remoteAddress, uint16 remoteConnectionID)
{
	return remoteAddress.getHash() ^ remoteConnectionID;
}

NetConnection::~NetConnection()
{
	clear();
}

void NetConnection::clear()
{
	mState = State::EMPTY;
	mTimeoutStart = 0;

	for (auto& pair : mOpenRequests)
	{
		pair.second->mRegisteredAtConnection = nullptr;
	}
	mOpenRequests.clear();

	if (nullptr != mConnectionManager)
	{
		mConnectionManager->removeConnection(*this);
		mConnectionManager = nullptr;
	}

	mSentPacketCache.clear();
	mReceivedPacketCache.clear();
}

UDPSocket* NetConnection::getSocket() const
{
	return (nullptr == mConnectionManager) ? nullptr : &mConnectionManager->getSocket();
}

void NetConnection::setProtocolVersions(uint8 lowLevelProtocolVersion, uint8 highLevelProtocolVersion)
{
	mLowLevelProtocolVersion = lowLevelProtocolVersion;
	mHighLevelProtocolVersion = highLevelProtocolVersion;
}

bool NetConnection::startConnectTo(ConnectionManager& connectionManager, const SocketAddress& remoteAddress)
{
	clear();

	mState = State::REQUESTED_CONNECTION;
	mConnectionManager = &connectionManager;
	mLocalConnectionID = 0;			// Not yet set, see "addConnection" below
	mRemoteConnectionID = 0;		// Not yet set
	mRemoteAddress = remoteAddress;
	mSenderKey = 0;					// Not yet set as it depends on the remote connection ID

	mConnectionManager->addConnection(*this);	// This will also set the local connection ID

	// Send a low-level message to establish the connection
	{
		std::cout << "Starting connection to " << mRemoteAddress.toString() << std::endl;

		// Get a new packet instance to fill
		SentPacket& sentPacket = mConnectionManager->rentSentPacket();

		// Build packet
		lowlevel::StartConnectionPacket packet;
		packet.mLowLevelMinimumProtocolVersion = lowlevel::PacketBase::LOWLEVEL_MINIMUM_PROTOCOL_VERSION;
		packet.mLowLevelMaximumProtocolVersion = lowlevel::PacketBase::LOWLEVEL_MAXIMUM_PROTOCOL_VERSION;
		packet.mHighLevelMinimumProtocolVersion = mConnectionManager->getHighLevelMinimumProtocolVersion();
		packet.mHighLevelMaximumProtocolVersion = mConnectionManager->getHighLevelMaximumProtocolVersion();

		// And send it
		if (!sendLowLevelPacket(packet, sentPacket.mContent))
		{
			sentPacket.returnToPool();
			return false;
		}

		// Add the packet to the cache, so it can be resent if needed
		mSentPacketCache.addPacket(sentPacket, mCurrentTimestamp, true);
	}
	return true;
}

bool NetConnection::isConnectedTo(uint16 localConnectionID, uint16 remoteConnectionID, uint64 senderKey) const
{
	return (nullptr != mConnectionManager && mState == State::CONNECTED && localConnectionID == mLocalConnectionID && remoteConnectionID == mRemoteConnectionID && senderKey == mSenderKey);
}

void NetConnection::disconnect(DisconnectReason disconnectReason)
{
	clear();
	mState = State::DISCONNECTED;
	mDisconnectReason = disconnectReason;
}

bool NetConnection::sendPacket(highlevel::PacketBase& packet)
{
	uint32 unused;
	return sendHighLevelPacket(packet, 0, unused);
}

bool NetConnection::sendRequest(highlevel::RequestBase& request)
{
	if (nullptr != request.mRegisteredAtConnection)
	{
		RMX_ASSERT(false, "Request can't get sent twice");
		return false;
	}

	request.mState = highlevel::RequestBase::State::SENT;

	// Send query packet
	lowlevel::RequestQueryPacket highLevelPacket;
	if (!sendHighLevelPacket(highLevelPacket, request.getQueryPacket(), 0, request.mUniqueRequestID))
		return false;

	// Register here
	mOpenRequests[request.mUniqueRequestID] = &request;
	request.mRegisteredAtConnection = this;
	return true;
}

bool NetConnection::respondToRequest(highlevel::RequestBase& request, uint32 uniqueRequestID)
{
	lowlevel::RequestResponsePacket highLevelPacket;
	highLevelPacket.mUniqueRequestID = uniqueRequestID;
	uint32 unused;
	return sendHighLevelPacket(highLevelPacket, request.getResponsePacket(), 0, unused);
}

bool NetConnection::readPacket(highlevel::PacketBase& packet, VectorBinarySerializer& serializer) const
{
	return packet.serializePacket(serializer, mHighLevelProtocolVersion);
}

void NetConnection::updateConnection(uint64 currentTimestamp)
{
	mCurrentTimestamp = currentTimestamp;

	// Update timeout
	if (mSentPacketCache.hasUnconfirmedPackets() && mTimeoutStart != 0)
	{
		if (mCurrentTimestamp >= mTimeoutStart + TIMEOUT_SECONDS * 1000)
		{
			// Trigger timeout
			std::cout << "Disconnect due to timeout" << std::endl;
			disconnect(DisconnectReason::TIMEOUT);
			return;
		}
	}
	else
	{
		// Not waiting for any confirmations, so reset timeout
		mTimeoutStart = mCurrentTimestamp;
	}

	// Update resending
	mPacketsToResend.clear();
	mSentPacketCache.updateResend(mPacketsToResend, currentTimestamp);

	for (const SentPacket* sentPacket : mPacketsToResend)
	{
		sendPacketInternal(sentPacket->mContent);
	}

	// TODO: Send a heartbeat every now and then
	//  -> But only if a heartbeat is even needed
	//  -> E.g. it can be omitted for a connection to a public server that does not send other packets than direct responses

}

void NetConnection::acceptIncomingConnection(ConnectionManager& connectionManager, uint16 remoteConnectionID, const SocketAddress& remoteAddress, uint64 senderKey)
{
	clear();

	mState = State::CONNECTED;
	mConnectionManager = &connectionManager;
	mLocalConnectionID = 0;			// Not yet set, see "addConnection" below
	mRemoteConnectionID = remoteConnectionID;
	mRemoteAddress = remoteAddress;
	mSenderKey = senderKey;
	RMX_ASSERT(senderKey == buildSenderKey(mRemoteAddress, mRemoteConnectionID), "Previously calculated sender key is the wrong one");

	std::cout << "Accepting connection from " << mRemoteAddress.toString() << std::endl;
	mConnectionManager->addConnection(*this);	// This will also set the local connection ID

	// Send back a response
	sendAcceptConnectionPacket();
}

void NetConnection::sendAcceptConnectionPacket()
{
	lowlevel::AcceptConnectionPacket packet;
	packet.mLowLevelProtocolVersion = mLowLevelProtocolVersion;
	packet.mHighLevelProtocolVersion = mHighLevelProtocolVersion;
	sendLowLevelPacket(packet, mSendBuffer);
}

void NetConnection::handleLowLevelPacket(ReceivedPacket& receivedPacket)
{
	// Reset timeout whenever any packet got received
	mTimeoutStart = mCurrentTimestamp;
	mLastMessageReceivedTimestamp = mCurrentTimestamp;	// TODO: It would be nice to use the actual timestamp of receiving the packet here, which happened previously already

	VectorBinarySerializer serializer(true, receivedPacket.mContent);
	serializer.skip(6);		// Skip low level signature and connection IDs, they got evaluated already
	if (serializer.getRemaining() <= 0)
		return;

	switch (receivedPacket.mLowLevelSignature)
	{
		case lowlevel::AcceptConnectionPacket::SIGNATURE:
		{
			lowlevel::AcceptConnectionPacket packet;
			if (!packet.serializePacket(serializer, lowlevel::PacketBase::LOWLEVEL_MINIMUM_PROTOCOL_VERSION))
				return;

			if (mState != State::REQUESTED_CONNECTION)
			{
				// Seems like something went wrong during establishing a connection, or maybe this was just a duplicate AcceptConnectionPacket
				return;
			}

			// Check if protocol versions are really supported
			if (packet.mLowLevelProtocolVersion < lowlevel::PacketBase::LOWLEVEL_MINIMUM_PROTOCOL_VERSION || packet.mLowLevelProtocolVersion > lowlevel::PacketBase::LOWLEVEL_MAXIMUM_PROTOCOL_VERSION ||
				packet.mHighLevelProtocolVersion < mConnectionManager->getHighLevelMinimumProtocolVersion() || packet.mHighLevelProtocolVersion > mConnectionManager->getHighLevelMaximumProtocolVersion())
			{
				std::cout << "Received accept connection packet with unsupported protocol version (low-level = " << packet.mLowLevelProtocolVersion << ", high-level = " << packet.mLowLevelProtocolVersion << ")" << std::endl;
				// TODO: Send back an error?
				return;
			}
			setProtocolVersions(packet.mLowLevelProtocolVersion, packet.mHighLevelProtocolVersion);

			std::cout << "Connection established to " << mRemoteAddress.toString() << std::endl;

			// Set remote connection ID, as it was not known before
			//  -> Unfortunately, we have to get in a somewhat awkward way, as it was skipped in deserialization before
			mRemoteConnectionID = *(uint16*)serializer.getBufferPointer(2);
			mSenderKey = buildSenderKey(mRemoteAddress, mRemoteConnectionID);
			mState = State::CONNECTED;

			// Stop resending the StartConnectionPacket
			mSentPacketCache.onPacketReceiveConfirmed(0);
			return;
		}

		case lowlevel::HighLevelPacket::SIGNATURE:
		case lowlevel::RequestQueryPacket::SIGNATURE:	// Can be treated the same way, as it does not have any additional members
		{
			lowlevel::HighLevelPacket highLevelPacket;
			if (!highLevelPacket.serializePacket(serializer, mLowLevelProtocolVersion))
				return;

			if (mState != State::CONNECTED)
			{
				std::cout << "Received high-level packet while not connected from " << mRemoteAddress.toString() << std::endl;
				return;
			}

			// Continue with specialized handling
			handleHighLevelPacket(receivedPacket, highLevelPacket, serializer, 0);
			return;
		}

		case lowlevel::RequestResponsePacket::SIGNATURE:
		{
			lowlevel::RequestResponsePacket requestResponsePacket;
			if (!requestResponsePacket.serializePacket(serializer, mLowLevelProtocolVersion))
				return;

			if (mState != State::CONNECTED)
			{
				std::cout << "Received high-level packet while not connected from " << mRemoteAddress.toString() << std::endl;
				return;
			}

			// Continue with specialized handling
			handleHighLevelPacket(receivedPacket, requestResponsePacket, serializer, requestResponsePacket.mUniqueRequestID);
			return;
		}

		case lowlevel::ReceiveConfirmationPacket::SIGNATURE:
		{
			lowlevel::ReceiveConfirmationPacket packet;
			if (!packet.serializePacket(serializer, mLowLevelProtocolVersion))
				return;

			// Packet was confirmed by the receiver, so remove it from the cache for re-sending
			mSentPacketCache.onPacketReceiveConfirmed(packet.mUniquePacketID);
			return;
		}
	}
}

void NetConnection::unregisterRequest(highlevel::RequestBase& request)
{
	mOpenRequests.erase(request.mUniqueRequestID);
}

bool NetConnection::sendPacketInternal(const std::vector<uint8>& content)
{
	if (nullptr == mConnectionManager)
		return false;

	mLastMessageSentTimestamp = mCurrentTimestamp;
	return mConnectionManager->sendPacketData(content, mRemoteAddress);
}

void NetConnection::writeLowLevelPacketContent(VectorBinarySerializer& serializer, lowlevel::PacketBase& lowLevelPacket)
{
	// Write shared header for all low-level packets
	serializer.write(lowLevelPacket.getSignature());
	serializer.write(mLocalConnectionID);
	serializer.write(mRemoteConnectionID);
	lowLevelPacket.serializePacket(serializer, mLowLevelProtocolVersion);
}

bool NetConnection::sendLowLevelPacket(lowlevel::PacketBase& lowLevelPacket, std::vector<uint8>& buffer)
{
	// Write low-level packet header
	buffer.clear();
	VectorBinarySerializer serializer(false, buffer);
	writeLowLevelPacketContent(serializer, lowLevelPacket);

	// And send it
	return sendPacketInternal(buffer);
}

bool NetConnection::sendHighLevelPacket(highlevel::PacketBase& highLevelPacket, uint8 flags, uint32& outUniquePacketID)
{
	// Build the low-level packet header for a generic high-level packet
	//  -> This header has no special members of its own, only the shared ones
	lowlevel::HighLevelPacket lowLevelPacket;
	return sendHighLevelPacket(lowLevelPacket, highLevelPacket, flags, outUniquePacketID);
}

bool NetConnection::sendHighLevelPacket(lowlevel::HighLevelPacket& lowLevelPacket, highlevel::PacketBase& highLevelPacket, uint8 flags, uint32& outUniquePacketID)
{
	lowLevelPacket.mPacketType = highLevelPacket.getPacketType();
	lowLevelPacket.mPacketFlags = flags;

	if (highLevelPacket.isReliablePacket())
	{
		lowLevelPacket.mUniquePacketID = mSentPacketCache.getNextUniquePacketID();

		// Get a new packet instance to fill
		SentPacket& sentPacket = mConnectionManager->rentSentPacket();

		// Write low-level packet header
		sentPacket.mContent.clear();
		VectorBinarySerializer serializer(false, sentPacket.mContent);
		writeLowLevelPacketContent(serializer, lowLevelPacket);

		// Now for the high-level packet content
		highLevelPacket.serializePacket(serializer, mHighLevelProtocolVersion);

		// And send it
		if (!sendPacketInternal(sentPacket.mContent))
		{
			sentPacket.returnToPool();
			return false;
		}

		// Add the packet to the cache, so it can be resent if needed
		mSentPacketCache.addPacket(sentPacket, mCurrentTimestamp);
	}
	else
	{
		lowLevelPacket.mUniquePacketID = 0;

		// Write low-level packet header
		mSendBuffer.clear();
		VectorBinarySerializer serializer(false, mSendBuffer);
		writeLowLevelPacketContent(serializer, lowLevelPacket);

		// Now for the high-level packet content
		highLevelPacket.serializePacket(serializer, mHighLevelProtocolVersion);

		// And send it
		if (!sendPacketInternal(mSendBuffer))
			return false;
	}

	outUniquePacketID = lowLevelPacket.mUniquePacketID;
	return true;
}

void NetConnection::handleHighLevelPacket(ReceivedPacket& receivedPacket, const lowlevel::HighLevelPacket& highLevelPacket, VectorBinarySerializer& serializer, uint32 uniqueResponseID)
{
	// Is this a tracked packet at all?
	const bool isTracked = (highLevelPacket.mUniquePacketID != 0);
	if (isTracked)
	{
		// In any case, send a confirmation to tell the sender that the tracked packet was received
		//  -> This way the sender knows it does not need to re-send it
		// TODO: It might make sense to send back a single confirmation for all tracked packets received in this update round, lowering overhead in case we got multiple of them at once
		{
			lowlevel::ReceiveConfirmationPacket packet;
			packet.mUniquePacketID = highLevelPacket.mUniquePacketID;
			sendLowLevelPacket(packet, mSendBuffer);
		}

		// Add to / check against queue of received packets
		const bool wasEnqueued = mReceivedPacketCache.enqueuePacket(receivedPacket, highLevelPacket, serializer, uniqueResponseID);
		if (!wasEnqueued)
		{
			// The packet is a duplicate we received already before, so ignore it
			return;
		}

		// Try extracting packets; possibly more than one
		ReceivedPacketCache::CacheItem extracted;
		while (mReceivedPacketCache.extractPacket(extracted))
		{
			processExtractedHighLevelPacket(extracted);
			
			// Remove the reference we took over from the ReceivedPacketCache
			extracted.mReceivedPacket->decReferenceCounter();
		}
	}
	else
	{
		// Not tracked: Simply forward it as-is to the listener
		ReceivedPacketEvaluation evaluation(*this, highLevelPacket.mPacketType, serializer);
		mConnectionManager->getListener().onReceivedPacket(evaluation);
	}
}

void NetConnection::processExtractedHighLevelPacket(const ReceivedPacketCache::CacheItem& extracted)
{
	VectorBinarySerializer newSerializer(true, extracted.mReceivedPacket->mContent);
	newSerializer.skip(extracted.mHeaderSize);

	switch (extracted.mReceivedPacket->mLowLevelSignature)
	{
		case lowlevel::HighLevelPacket::SIGNATURE:
		default:
		{
			// Generic high-level packet
			ReceivedPacketEvaluation evaluation(*this, extracted.mPacketHeader.mPacketType, newSerializer);
			mConnectionManager->getListener().onReceivedPacket(evaluation);
			break;
		}

		case lowlevel::RequestQueryPacket::SIGNATURE:
		{
			// Request query
			ReceivedQueryEvaluation evaluation(*this, extracted.mPacketHeader.mPacketType, newSerializer, extracted.mPacketHeader.mUniquePacketID);
			if (!mConnectionManager->getListener().onReceivedRequestQuery(evaluation))
			{
				// TODO: Send back an error packet, this request won't receive a response
				//  -> This needs to be a tracked high-level packet
			}
			break;
		}

		case lowlevel::RequestResponsePacket::SIGNATURE:
		{
			// Response to a request
			const auto it = mOpenRequests.find(extracted.mUniqueRequestID);
			if (it != mOpenRequests.end())
			{
				highlevel::RequestBase& request = *it->second;
				if (request.getResponsePacket().serializePacket(newSerializer, mHighLevelProtocolVersion))
				{
					request.mState = highlevel::RequestBase::State::SUCCESS;

					// Also inform the listener
					ReceivedRequestEvaluation evaluation(*this, request);
					mConnectionManager->getListener().onReceivedRequestResponse(evaluation);
				}
				else
				{
					request.mState = highlevel::RequestBase::State::FAILED;

					// Also inform the listener
					ReceivedRequestEvaluation evaluation(*this, request);
					mConnectionManager->getListener().onReceivedRequestError(evaluation);
				}
				mOpenRequests.erase(it);
			}
			break;
		}
	}
}
