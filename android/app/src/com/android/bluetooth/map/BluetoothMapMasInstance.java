/*
 * Copyright (C) 2014 Samsung System LSI
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.android.bluetooth.map;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothProtoEnums;
import android.bluetooth.BluetoothSocket;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.RemoteException;
import android.os.SystemProperties;
import android.util.Log;

import com.android.bluetooth.BluetoothObexTransport;
import com.android.bluetooth.BluetoothStatsLog;
import com.android.bluetooth.IObexConnectionHandler;
import com.android.bluetooth.ObexServerSockets;
import com.android.bluetooth.Utils;
import com.android.bluetooth.content_profiles.ContentProfileErrorReportUtils;
import com.android.bluetooth.map.BluetoothMapContentObserver.Msg;
import com.android.bluetooth.map.BluetoothMapUtils.TYPE;
import com.android.bluetooth.sdp.SdpManagerNativeInterface;
import com.android.internal.annotations.VisibleForTesting;
import com.android.obex.ServerSession;

import java.io.IOException;
import java.util.Calendar;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

// Next tag value for ContentProfileErrorReportUtils.report(): 4
public class BluetoothMapMasInstance implements IObexConnectionHandler {
    private static final String TAG = "BluetoothMapMasInstance";

    @VisibleForTesting static volatile int sInstanceCounter = 0;

    private final int mObjectInstanceId;

    private static final int SDP_MAP_MSG_TYPE_EMAIL = 0x01;
    private static final int SDP_MAP_MSG_TYPE_SMS_GSM = 0x02;
    private static final int SDP_MAP_MSG_TYPE_SMS_CDMA = 0x04;
    private static final int SDP_MAP_MSG_TYPE_MMS = 0x08;
    private static final int SDP_MAP_MSG_TYPE_IM = 0x10;

    private static final String BLUETOOTH_MAP_VERSION_PROPERTY = "persist.bluetooth.mapversion";

    private static final int SDP_MAP_MAS_VERSION_1_2 = 0x0102;
    private static final int SDP_MAP_MAS_VERSION_1_3 = 0x0103;
    private static final int SDP_MAP_MAS_VERSION_1_4 = 0x0104;

    /* TODO: Should these be adaptive for each MAS? - e.g. read from app? */
    static final int SDP_MAP_MAS_FEATURES_1_2 = 0x0000007F;
    static final int SDP_MAP_MAS_FEATURES_1_3 = 0x000603FF;
    static final int SDP_MAP_MAS_FEATURES_1_4 = 0x000603FF;

    // PTS requires Bit 19 = MapSupportedFeatures in Connect Request
    static final int SDP_MAP_MAS_FEATURES_1_4_PTS = 0x000E03FF;

    private ServerSession mServerSession = null;
    // The handle to the socket registration with SDP
    private ObexServerSockets mServerSockets = null;
    private int mSdpHandle = -1;

    // The actual incoming connection handle
    private BluetoothSocket mConnSocket = null;
    // The remote connected device
    private BluetoothAdapter mAdapter;

    private volatile boolean mShutdown = false; // Used to interrupt socket accept thread
    private volatile boolean mAcceptNewConnections = false;

    private Handler mServiceHandler = null; // MAP service message handler
    private BluetoothMapService mMapService = null; // Handle to the outer MAP service
    private Context mContext = null; // MAP service context
    private BluetoothMnsObexClient mMnsClient = null; // Shared MAP MNS client
    private BluetoothMapAccountItem mAccount = null; //
    private String mBaseUri = null; // Client base URI for this instance
    private int mMasInstanceId = -1;
    private boolean mEnableSmsMms = false;
    BluetoothMapContentObserver mObserver;
    private BluetoothMapObexServer mMapServer;
    private AtomicLong mDbIndetifier = new AtomicLong();
    private AtomicLong mFolderVersionCounter = new AtomicLong(0);
    private AtomicLong mSmsMmsConvoListVersionCounter = new AtomicLong(0);
    private AtomicLong mImEmailConvoListVersionCounter = new AtomicLong(0);

    private Map<Long, Msg> mMsgListSms = null;
    private Map<Long, Msg> mMsgListMms = null;
    private Map<Long, Msg> mMsgListMsg = null;

    private Map<String, BluetoothMapConvoContactElement> mContactList;

    private HashMap<Long, BluetoothMapConvoListingElement> mSmsMmsConvoList =
            new HashMap<Long, BluetoothMapConvoListingElement>();

    private HashMap<Long, BluetoothMapConvoListingElement> mImEmailConvoList =
            new HashMap<Long, BluetoothMapConvoListingElement>();

    private int mRemoteFeatureMask = BluetoothMapUtils.MAP_FEATURE_DEFAULT_BITMASK;
    private static int sFeatureMask = SDP_MAP_MAS_FEATURES_1_4;

    public static final String TYPE_SMS_MMS_STR = "SMS/MMS";
    public static final String TYPE_EMAIL_STR = "EMAIL";
    public static final String TYPE_IM_STR = "IM";

    /** Create a e-mail MAS instance */
    public BluetoothMapMasInstance(
            BluetoothMapService mapService,
            Context context,
            BluetoothMapAccountItem account,
            int masId,
            boolean enableSmsMms) {
        mObjectInstanceId = sInstanceCounter++;
        mMapService = mapService;
        mServiceHandler = mapService.getHandler();
        mContext = context;
        mAccount = account;
        if (account != null) {
            mBaseUri = account.mBase_uri;
        }
        mMasInstanceId = masId;
        mEnableSmsMms = enableSmsMms;
        init();
    }

    private void removeSdpRecord() {
        SdpManagerNativeInterface nativeInterface = SdpManagerNativeInterface.getInstance();
        if (mAdapter != null && mSdpHandle >= 0 && nativeInterface.isAvailable()) {
            verbose(
                    "Removing SDP record for MAS instance: "
                            + mMasInstanceId
                            + " Object reference: "
                            + this
                            + ", SDP handle: "
                            + mSdpHandle);
            boolean status = nativeInterface.removeSdpRecord(mSdpHandle);
            debug("RemoveSDPrecord returns " + status);
            mSdpHandle = -1;
        }
    }

    @Override
    public String toString() {
        return "MasId: " + mMasInstanceId + " Uri:" + mBaseUri + " SMS/MMS:" + mEnableSmsMms;
    }

    private void init() {
        mAdapter = BluetoothAdapter.getDefaultAdapter();
    }

    /**
     * The data base identifier is used by connecting MCE devices to evaluate if cached data is
     * still valid, hence only update this value when something actually invalidates the data.
     * Situations where this must be called: - MAS ID's vs. server channels are scrambled (As
     * neither MAS ID, name or server channels) can be used by a client to uniquely identify a
     * specific message database - except MAS id 0 we should change this value if the server channel
     * is changed. - If a MAS instance folderVersionCounter roles over - will not happen before a
     * long is too small to hold a unix time-stamp, hence is not handled.
     */
    private void updateDbIdentifier() {
        mDbIndetifier.set(Calendar.getInstance().getTime().getTime());
    }

    /**
     * update the time stamp used for FOLDER version counter. Call once when a content provider
     * notification caused applicable changes to the list of messages.
     */
    /* package */ void updateFolderVersionCounter() {
        mFolderVersionCounter.incrementAndGet();
    }

    /**
     * update the CONVO LIST version counter. Call once when a content provider notification caused
     * applicable changes to the list of contacts, or when an update is manually triggered.
     */
    /* package */ void updateSmsMmsConvoListVersionCounter() {
        mSmsMmsConvoListVersionCounter.incrementAndGet();
    }

    /* package */ void updateImEmailConvoListVersionCounter() {
        mImEmailConvoListVersionCounter.incrementAndGet();
    }

    /* package */ Map<Long, Msg> getMsgListSms() {
        return mMsgListSms;
    }

    /* package */ void setMsgListSms(Map<Long, Msg> msgListSms) {
        mMsgListSms = msgListSms;
    }

    /* package */ Map<Long, Msg> getMsgListMms() {
        return mMsgListMms;
    }

    /* package */ void setMsgListMms(Map<Long, Msg> msgListMms) {
        mMsgListMms = msgListMms;
    }

    /* package */ Map<Long, Msg> getMsgListMsg() {
        return mMsgListMsg;
    }

    /* package */ void setMsgListMsg(Map<Long, Msg> msgListMsg) {
        mMsgListMsg = msgListMsg;
    }

    /* package */ Map<String, BluetoothMapConvoContactElement> getContactList() {
        return mContactList;
    }

    /* package */ void setContactList(Map<String, BluetoothMapConvoContactElement> contactList) {
        mContactList = contactList;
    }

    HashMap<Long, BluetoothMapConvoListingElement> getSmsMmsConvoList() {
        return mSmsMmsConvoList;
    }

    void setSmsMmsConvoList(HashMap<Long, BluetoothMapConvoListingElement> smsMmsConvoList) {
        mSmsMmsConvoList = smsMmsConvoList;
    }

    HashMap<Long, BluetoothMapConvoListingElement> getImEmailConvoList() {
        return mImEmailConvoList;
    }

    void setImEmailConvoList(HashMap<Long, BluetoothMapConvoListingElement> imEmailConvoList) {
        mImEmailConvoList = imEmailConvoList;
    }

    /* package*/
    int getMasId() {
        return mMasInstanceId;
    }

    /* package*/
    long getDbIdentifier() {
        return mDbIndetifier.get();
    }

    /* package*/
    long getFolderVersionCounter() {
        return mFolderVersionCounter.get();
    }

    /* package */
    long getCombinedConvoListVersionCounter() {
        long combinedVersionCounter = mSmsMmsConvoListVersionCounter.get();
        combinedVersionCounter += mImEmailConvoListVersionCounter.get();
        return combinedVersionCounter;
    }

    /** Start Obex Server Sockets and create the SDP record. */
    public synchronized void startSocketListeners() {
        debug("Map Service startSocketListeners");

        if (mServerSession != null) {
            debug("mServerSession exists - shutting it down...");
            mServerSession.close();
            mServerSession = null;
        }
        if (mObserver != null) {
            debug("mObserver exists - shutting it down...");
            mObserver.deinit();
            mObserver = null;
        }

        closeConnectionSocket();

        if (mServerSockets != null) {
            mAcceptNewConnections = true;
        } else {

            mServerSockets = ObexServerSockets.create(this);
            mAcceptNewConnections = true;

            if (mServerSockets == null) {
                // TODO: Handle - was not handled before
                error("Failed to start the listeners");
                ContentProfileErrorReportUtils.report(
                        BluetoothProfile.MAP,
                        BluetoothProtoEnums.BLUETOOTH_MAP_MAS_INSTANCE,
                        BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_ERROR,
                        0);
                return;
            }
            removeSdpRecord();
            mSdpHandle =
                    createMasSdpRecord(
                            mServerSockets.getRfcommChannel(), mServerSockets.getL2capPsm());
            // Here we might have changed crucial data, hence reset DB identifier
            verbose(
                    "Creating new SDP record for MAS instance: "
                            + mMasInstanceId
                            + " Object reference: "
                            + this
                            + ", SDP handle: "
                            + mSdpHandle);
            updateDbIdentifier();
        }
    }

    /**
     * Create the MAS SDP record with the information stored in the instance.
     *
     * @param rfcommChannel the rfcomm channel ID
     * @param l2capPsm the l2capPsm - set to -1 to exclude
     */
    private int createMasSdpRecord(int rfcommChannel, int l2capPsm) {
        String masName = "";
        int messageTypeFlags = 0;
        if (mEnableSmsMms) {
            masName = TYPE_SMS_MMS_STR;
            messageTypeFlags |=
                    SDP_MAP_MSG_TYPE_SMS_GSM | SDP_MAP_MSG_TYPE_SMS_CDMA | SDP_MAP_MSG_TYPE_MMS;
        }

        if (mBaseUri != null) {
            if (mEnableSmsMms) {
                if (mAccount.getType() == TYPE.EMAIL) {
                    masName += "/" + TYPE_EMAIL_STR;
                } else if (mAccount.getType() == TYPE.IM) {
                    masName += "/" + TYPE_IM_STR;
                }
            } else {
                masName = mAccount.getName();
            }

            if (mAccount.getType() == TYPE.EMAIL) {
                messageTypeFlags |= SDP_MAP_MSG_TYPE_EMAIL;
            } else if (mAccount.getType() == TYPE.IM) {
                messageTypeFlags |= SDP_MAP_MSG_TYPE_IM;
            }
        }

        final String currentValue = SystemProperties.get(BLUETOOTH_MAP_VERSION_PROPERTY, "map14");
        int masVersion;

        switch (currentValue) {
            case "map12":
                masVersion = SDP_MAP_MAS_VERSION_1_2;
                sFeatureMask = SDP_MAP_MAS_FEATURES_1_2;
                break;
            case "map13":
                masVersion = SDP_MAP_MAS_VERSION_1_3;
                sFeatureMask = SDP_MAP_MAS_FEATURES_1_3;
                break;
            case "map14":
                masVersion = SDP_MAP_MAS_VERSION_1_4;
                sFeatureMask = (Utils.isPtsTestMode() ? SDP_MAP_MAS_FEATURES_1_4_PTS :
                    SDP_MAP_MAS_FEATURES_1_4);
                break;
            default:
                masVersion = SDP_MAP_MAS_VERSION_1_4;
                sFeatureMask = SDP_MAP_MAS_FEATURES_1_4;
        }

        return SdpManagerNativeInterface.getInstance()
                .createMapMasRecord(
                        masName,
                        mMasInstanceId,
                        rfcommChannel,
                        l2capPsm,
                        masVersion,
                        messageTypeFlags,
                        sFeatureMask);
    }

    /* Called for all MAS instances for each instance when auth. is completed, hence
     * must check if it has a valid connection before creating a session.
     * Returns true at success. */
    public boolean startObexServerSession(BluetoothMnsObexClient mnsClient)
            throws IOException, RemoteException {
        debug("Map Service startObexServerSession masid = " + mMasInstanceId);

        if (mConnSocket != null) {
            if (mServerSession != null) {
                // Already connected, just return true
                return true;
            }

            mMnsClient = mnsClient;
            mObserver =
                    new BluetoothMapContentObserver(
                            mContext, mMnsClient, this, mAccount, mEnableSmsMms);
            mObserver.init();
            mMapServer =
                    new BluetoothMapObexServer(
                            mServiceHandler, mContext, mObserver, this, mAccount, mEnableSmsMms);
            mMapServer.setRemoteFeatureMask(mRemoteFeatureMask);
            // setup transport
            BluetoothObexTransport transport = new BluetoothObexTransport(mConnSocket);
            mServerSession = new ServerSession(transport, mMapServer, null);
            debug("    ServerSession started.");

            return true;
        }
        debug("    No connection for this instance");
        return false;
    }

    public boolean handleSmsSendIntent(Context context, Intent intent) {
        if (mObserver != null) {
            return mObserver.handleSmsSendIntent(context, intent);
        }
        return false;
    }

    /**
     * Check if this instance is started.
     *
     * @return true if started
     */
    public boolean isStarted() {
        return (mConnSocket != null);
    }

    public void shutdown() {
        debug("MAP Service shutdown");

        if (mServerSession != null) {
            mServerSession.close();
            mServerSession = null;
        }
        if (mObserver != null) {
            mObserver.deinit();
            mObserver = null;
        }

        removeSdpRecord();

        closeConnectionSocket();
        // Do not block for Accept thread cleanup.
        // Fix Handler Thread block during BT Turn OFF.
        closeServerSockets(false);
    }

    /** Signal to the ServerSockets handler that a new connection may be accepted. */
    public void restartObexServerSession() {
        debug("MAP Service restartObexServerSession()");
        startSocketListeners();
    }

    private synchronized void closeServerSockets(boolean block) {
        // exit SocketAcceptThread early
        ObexServerSockets sockets = mServerSockets;
        if (sockets != null) {
            sockets.shutdown(block);
            mServerSockets = null;
        }
    }

    private synchronized void closeConnectionSocket() {
        if (mConnSocket != null) {
            try {
                mConnSocket.close();
            } catch (IOException e) {
                ContentProfileErrorReportUtils.report(
                        BluetoothProfile.MAP,
                        BluetoothProtoEnums.BLUETOOTH_MAP_MAS_INSTANCE,
                        BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__EXCEPTION,
                        1);
                error("Close Connection Socket error: ", e);
            } finally {
                mConnSocket = null;
            }
        }
    }

    public void setRemoteFeatureMask(int supportedFeatures) {
        verbose("setRemoteFeatureMask : Curr: " + mRemoteFeatureMask);
        mRemoteFeatureMask = supportedFeatures & sFeatureMask;
        BluetoothMapUtils.savePeerSupportUtcTimeStamp(mRemoteFeatureMask);
        if (mObserver != null) {
            mObserver.setObserverRemoteFeatureMask(mRemoteFeatureMask);
            mMapServer.setRemoteFeatureMask(mRemoteFeatureMask);
            verbose("setRemoteFeatureMask : set: " + mRemoteFeatureMask);
        }
    }

    public int getRemoteFeatureMask() {
        return this.mRemoteFeatureMask;
    }

    public static int getFeatureMask() {
        return sFeatureMask;
    }

    @Override
    public synchronized boolean onConnect(BluetoothDevice device, BluetoothSocket socket) {
        if (!mAcceptNewConnections) {
            return false;
        }
        /* Signal to the service that we have received an incoming connection.
         */
        boolean isValid = mMapService.onConnect(device, BluetoothMapMasInstance.this);

        if (isValid) {
            mConnSocket = socket;
            mAcceptNewConnections = false;
        }

        return isValid;
    }

    /**
     * Called when an unrecoverable error occurred in an accept thread. Close down the server
     * socket, and restart. TODO: Change to message, to call start in correct context.
     */
    @Override
    public synchronized void onAcceptFailed() {
        mServerSockets = null; // Will cause a new to be created when calling start.
        if (mShutdown) {
            error("Failed to accept incoming connection - shutdown");
            ContentProfileErrorReportUtils.report(
                    BluetoothProfile.MAP,
                    BluetoothProtoEnums.BLUETOOTH_MAP_MAS_INSTANCE,
                    BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_ERROR,
                    2);
        } else {
            error("Failed to accept incoming connection - restarting");
            ContentProfileErrorReportUtils.report(
                    BluetoothProfile.MAP,
                    BluetoothProtoEnums.BLUETOOTH_MAP_MAS_INSTANCE,
                    BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_ERROR,
                    3);
            startSocketListeners();
        }
    }

    // Per-Instance logging
    public void verbose(String message) {
        Log.v(TAG, "[instance=" + mObjectInstanceId + "] " + message);
    }

    public void debug(String message) {
        Log.d(TAG, "[instance=" + mObjectInstanceId + "] " + message);
    }

    public void error(String message) {
        Log.e(TAG, "[instance=" + mObjectInstanceId + "] " + message);
    }

    public void error(String message, Exception e) {
        Log.e(TAG, "[instance=" + mObjectInstanceId + "] " + message, e);
    }
}
