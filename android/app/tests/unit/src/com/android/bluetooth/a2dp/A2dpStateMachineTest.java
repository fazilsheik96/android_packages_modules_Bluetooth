/*
 * Copyright 2017 The Android Open Source Project
 *
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

package com.android.bluetooth.a2dp;

import static com.google.common.truth.Truth.assertThat;

import static org.mockito.Mockito.*;

import android.bluetooth.BluetoothA2dp;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothCodecConfig;
import android.bluetooth.BluetoothCodecStatus;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothProfile;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.HandlerThread;

import androidx.test.InstrumentationRegistry;
import androidx.test.filters.MediumTest;
import androidx.test.runner.AndroidJUnit4;

import com.android.bluetooth.TestUtils;
import com.android.bluetooth.btservice.ActiveDeviceManager;
import com.android.bluetooth.btservice.AdapterService;
import com.android.bluetooth.btservice.SilenceDeviceManager;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;
import org.mockito.Mock;
import org.mockito.junit.MockitoJUnit;
import org.mockito.junit.MockitoRule;

import java.util.Arrays;

@MediumTest
@RunWith(AndroidJUnit4.class)
public class A2dpStateMachineTest {
    private BluetoothAdapter mAdapter;
    private HandlerThread mHandlerThread;
    private A2dpStateMachine mA2dpStateMachine;
    private BluetoothDevice mTestDevice;
    private static final int TIMEOUT_MS = 1000; // 1s

    private BluetoothCodecConfig mCodecConfigSbc;
    private BluetoothCodecConfig mCodecConfigAac;
    private BluetoothCodecConfig mCodecConfigOpus;

    @Rule public MockitoRule mockitoRule = MockitoJUnit.rule();

    @Mock private AdapterService mAdapterService;
    @Mock private ActiveDeviceManager mActiveDeviceManager;
    @Mock private SilenceDeviceManager mSilenceDeviceManager;
    @Mock private A2dpService mA2dpService;
    @Mock private A2dpNativeInterface mA2dpNativeInterface;

    @Before
    public void setUp() throws Exception {
        doReturn(mActiveDeviceManager).when(mAdapterService).getActiveDeviceManager();
        doReturn(mSilenceDeviceManager).when(mAdapterService).getSilenceDeviceManager();

        TestUtils.setAdapterService(mAdapterService);

        mAdapter = BluetoothAdapter.getDefaultAdapter();

        // Get a device for testing
        mTestDevice = mAdapter.getRemoteDevice("00:01:02:03:04:05");

        // Set up sample codec config
        mCodecConfigSbc =
                new BluetoothCodecConfig.Builder()
                        .setCodecType(BluetoothCodecConfig.SOURCE_CODEC_TYPE_SBC)
                        .setCodecPriority(BluetoothCodecConfig.CODEC_PRIORITY_DEFAULT)
                        .setSampleRate(BluetoothCodecConfig.SAMPLE_RATE_44100)
                        .setBitsPerSample(BluetoothCodecConfig.BITS_PER_SAMPLE_16)
                        .setChannelMode(BluetoothCodecConfig.CHANNEL_MODE_STEREO)
                        .setCodecSpecific1(0)
                        .setCodecSpecific2(0)
                        .setCodecSpecific3(0)
                        .setCodecSpecific4(0)
                        .build();
        mCodecConfigAac =
                new BluetoothCodecConfig.Builder()
                        .setCodecType(BluetoothCodecConfig.SOURCE_CODEC_TYPE_AAC)
                        .setCodecPriority(BluetoothCodecConfig.CODEC_PRIORITY_DEFAULT)
                        .setSampleRate(BluetoothCodecConfig.SAMPLE_RATE_48000)
                        .setBitsPerSample(BluetoothCodecConfig.BITS_PER_SAMPLE_16)
                        .setChannelMode(BluetoothCodecConfig.CHANNEL_MODE_STEREO)
                        .setCodecSpecific1(0)
                        .setCodecSpecific2(0)
                        .setCodecSpecific3(0)
                        .setCodecSpecific4(0)
                        .build();

        mCodecConfigOpus =
                new BluetoothCodecConfig.Builder()
                        .setCodecType(BluetoothCodecConfig.SOURCE_CODEC_TYPE_OPUS)
                        .setCodecPriority(BluetoothCodecConfig.CODEC_PRIORITY_DEFAULT)
                        .setSampleRate(BluetoothCodecConfig.SAMPLE_RATE_48000)
                        .setBitsPerSample(BluetoothCodecConfig.BITS_PER_SAMPLE_16)
                        .setChannelMode(BluetoothCodecConfig.CHANNEL_MODE_STEREO)
                        .setCodecSpecific1(0)
                        .setCodecSpecific2(0)
                        .setCodecSpecific3(0)
                        .setCodecSpecific4(0)
                        .build();

        // Set up thread and looper
        mHandlerThread = new HandlerThread("A2dpStateMachineTestHandlerThread");
        mHandlerThread.start();
        mA2dpStateMachine =
                new A2dpStateMachine(
                        mTestDevice,
                        mA2dpService,
                        mA2dpNativeInterface,
                        mHandlerThread.getLooper());
        // Override the timeout value to speed up the test
        A2dpStateMachine.sConnectTimeoutMs = 1000; // 1s
        mA2dpStateMachine.start();
    }

    @After
    public void tearDown() throws Exception {
        mA2dpStateMachine.doQuit();
        mHandlerThread.quit();
        mHandlerThread.join(TIMEOUT_MS);
        TestUtils.clearAdapterService(mAdapterService);
    }

    /** Test that default state is disconnected */
    @Test
    public void testDefaultDisconnectedState() {
        assertThat(mA2dpStateMachine.getConnectionState())
                .isEqualTo(BluetoothProfile.STATE_DISCONNECTED);
    }

    /**
     * Allow/disallow connection to any device.
     *
     * @param allow if true, connection is allowed
     */
    private void allowConnection(boolean allow) {
        doReturn(allow).when(mA2dpService).okToConnect(any(BluetoothDevice.class), anyBoolean());
    }

    /** Test that an incoming connection with low priority is rejected */
    @Test
    public void testIncomingPriorityReject() {
        allowConnection(false);

        // Inject an event for when incoming connection is requested
        A2dpStackEvent connStCh =
                new A2dpStackEvent(A2dpStackEvent.EVENT_TYPE_CONNECTION_STATE_CHANGED);
        connStCh.device = mTestDevice;
        connStCh.valueInt = A2dpStackEvent.CONNECTION_STATE_CONNECTED;
        mA2dpStateMachine.sendMessage(A2dpStateMachine.STACK_EVENT, connStCh);

        // Verify that no connection state broadcast is executed
        verify(mA2dpService, after(TIMEOUT_MS).never())
                .sendBroadcast(any(Intent.class), anyString(), any(Bundle.class));
        verify(mA2dpService, times(1)).disconnectAvrcp(mTestDevice);
        // Check that we are in Disconnected state
        assertThat(mA2dpStateMachine.getCurrentState())
                .isInstanceOf(A2dpStateMachine.Disconnected.class);
    }

    /** Test that an incoming connection with high priority is accepted */
    @Test
    public void testIncomingPriorityAccept() {
        allowConnection(true);

        // Inject an event for when incoming connection is requested
        A2dpStackEvent connStCh =
                new A2dpStackEvent(A2dpStackEvent.EVENT_TYPE_CONNECTION_STATE_CHANGED);
        connStCh.device = mTestDevice;
        connStCh.valueInt = A2dpStackEvent.CONNECTION_STATE_CONNECTING;
        mA2dpStateMachine.sendMessage(A2dpStateMachine.STACK_EVENT, connStCh);

        // Verify that one connection state broadcast is executed
        ArgumentCaptor<Intent> intentArgument1 = ArgumentCaptor.forClass(Intent.class);
        verify(mA2dpService, timeout(TIMEOUT_MS).times(1))
                .sendBroadcast(intentArgument1.capture(), anyString(), any(Bundle.class));
        assertThat(intentArgument1.getValue().getIntExtra(BluetoothProfile.EXTRA_STATE, -1))
                .isEqualTo(BluetoothProfile.STATE_CONNECTING);

        // Check that we are in Connecting state
        assertThat(mA2dpStateMachine.getCurrentState())
                .isInstanceOf(A2dpStateMachine.Connecting.class);

        // Send a message to trigger connection completed
        A2dpStackEvent connCompletedEvent =
                new A2dpStackEvent(A2dpStackEvent.EVENT_TYPE_CONNECTION_STATE_CHANGED);
        connCompletedEvent.device = mTestDevice;
        connCompletedEvent.valueInt = A2dpStackEvent.CONNECTION_STATE_CONNECTED;
        mA2dpStateMachine.sendMessage(A2dpStateMachine.STACK_EVENT, connCompletedEvent);

        // Verify that the expected number of broadcasts are executed:
        // - two calls to broadcastConnectionState(): Disconnected -> Connecting -> Connected
        // - one call to broadcastAudioState() when entering Connected state
        ArgumentCaptor<Intent> intentArgument2 = ArgumentCaptor.forClass(Intent.class);
        verify(mA2dpService, timeout(TIMEOUT_MS).times(3))
                .sendBroadcast(intentArgument2.capture(), anyString(), any(Bundle.class));
        // Verify that the last broadcast was to change the A2DP playing state
        // to STATE_NOT_PLAYING
        assertThat(intentArgument2.getValue().getAction())
                .isEqualTo(BluetoothA2dp.ACTION_PLAYING_STATE_CHANGED);
        assertThat(intentArgument2.getValue().getIntExtra(BluetoothProfile.EXTRA_STATE, -1))
                .isEqualTo(BluetoothA2dp.STATE_NOT_PLAYING);
        // Check that we are in Connected state
        assertThat(mA2dpStateMachine.getCurrentState())
                .isInstanceOf(A2dpStateMachine.Connected.class);
    }

    /** Test that an outgoing connection times out */
    @Test
    public void testOutgoingTimeout() {
        allowConnection(true);
        doReturn(true).when(mA2dpNativeInterface).connectA2dp(any(BluetoothDevice.class));
        doReturn(true).when(mA2dpNativeInterface).disconnectA2dp(any(BluetoothDevice.class));

        // Send a connect request
        mA2dpStateMachine.sendMessage(A2dpStateMachine.CONNECT, mTestDevice);

        // Verify that one connection state broadcast is executed
        ArgumentCaptor<Intent> intentArgument1 = ArgumentCaptor.forClass(Intent.class);
        verify(mA2dpService, timeout(TIMEOUT_MS).times(1))
                .sendBroadcast(intentArgument1.capture(), anyString(), any(Bundle.class));
        assertThat(intentArgument1.getValue().getIntExtra(BluetoothProfile.EXTRA_STATE, -1))
                .isEqualTo(BluetoothProfile.STATE_CONNECTING);

        // Check that we are in Connecting state
        assertThat(mA2dpStateMachine.getCurrentState())
                .isInstanceOf(A2dpStateMachine.Connecting.class);

        // Verify that one connection state broadcast is executed
        ArgumentCaptor<Intent> intentArgument2 = ArgumentCaptor.forClass(Intent.class);
        verify(mA2dpService, timeout(A2dpStateMachine.sConnectTimeoutMs * 2).times(2))
                .sendBroadcast(intentArgument2.capture(), anyString(), any(Bundle.class));
        assertThat(intentArgument2.getValue().getIntExtra(BluetoothProfile.EXTRA_STATE, -1))
                .isEqualTo(BluetoothProfile.STATE_DISCONNECTED);

        // Check that we are in Disconnected state
        assertThat(mA2dpStateMachine.getCurrentState())
                .isInstanceOf(A2dpStateMachine.Disconnected.class);
    }

    /** Test that an incoming connection times out */
    @Test
    public void testIncomingTimeout() {
        allowConnection(true);
        doReturn(true).when(mA2dpNativeInterface).connectA2dp(any(BluetoothDevice.class));
        doReturn(true).when(mA2dpNativeInterface).disconnectA2dp(any(BluetoothDevice.class));

        // Inject an event for when incoming connection is requested
        A2dpStackEvent connStCh =
                new A2dpStackEvent(A2dpStackEvent.EVENT_TYPE_CONNECTION_STATE_CHANGED);
        connStCh.device = mTestDevice;
        connStCh.valueInt = A2dpStackEvent.CONNECTION_STATE_CONNECTING;
        mA2dpStateMachine.sendMessage(A2dpStateMachine.STACK_EVENT, connStCh);

        // Verify that one connection state broadcast is executed
        ArgumentCaptor<Intent> intentArgument1 = ArgumentCaptor.forClass(Intent.class);
        verify(mA2dpService, timeout(TIMEOUT_MS).times(1))
                .sendBroadcast(intentArgument1.capture(), anyString(), any(Bundle.class));
        assertThat(intentArgument1.getValue().getIntExtra(BluetoothProfile.EXTRA_STATE, -1))
                .isEqualTo(BluetoothProfile.STATE_CONNECTING);

        // Check that we are in Connecting state
        assertThat(mA2dpStateMachine.getCurrentState())
                .isInstanceOf(A2dpStateMachine.Connecting.class);

        // Verify that one connection state broadcast is executed
        ArgumentCaptor<Intent> intentArgument2 = ArgumentCaptor.forClass(Intent.class);
        verify(mA2dpService, timeout(A2dpStateMachine.sConnectTimeoutMs * 2).times(2))
                .sendBroadcast(intentArgument2.capture(), anyString(), any(Bundle.class));
        assertThat(intentArgument2.getValue().getIntExtra(BluetoothProfile.EXTRA_STATE, -1))
                .isEqualTo(BluetoothProfile.STATE_DISCONNECTED);

        // Check that we are in Disconnected state
        assertThat(mA2dpStateMachine.getCurrentState())
                .isInstanceOf(A2dpStateMachine.Disconnected.class);
    }

    /** Test that codec config change been reported to A2dpService properly. */
    @Test
    public void testProcessCodecConfigEvent() {
        testProcessCodecConfigEventCase(false);
    }

    /**
     * Test that codec config change been reported to A2dpService properly when A2DP hardware
     * offloading is enabled.
     */
    @Test
    public void testProcessCodecConfigEvent_OffloadEnabled() {
        testProcessCodecConfigEventCase(true);
    }

    /** Helper methold to test processCodecConfigEvent() */
    public void testProcessCodecConfigEventCase(boolean offloadEnabled) {
        if (offloadEnabled) {
            mA2dpStateMachine.mA2dpOffloadEnabled = true;
        }

        doNothing()
                .when(mA2dpService)
                .codecConfigUpdated(
                        any(BluetoothDevice.class), any(BluetoothCodecStatus.class), anyBoolean());
        doNothing().when(mA2dpService).updateOptionalCodecsSupport(any(BluetoothDevice.class));
        allowConnection(true);

        BluetoothCodecConfig[] codecsSelectableSbc;
        codecsSelectableSbc = new BluetoothCodecConfig[1];
        codecsSelectableSbc[0] = mCodecConfigSbc;

        BluetoothCodecConfig[] codecsSelectableSbcAac;
        codecsSelectableSbcAac = new BluetoothCodecConfig[2];
        codecsSelectableSbcAac[0] = mCodecConfigSbc;
        codecsSelectableSbcAac[1] = mCodecConfigAac;

        BluetoothCodecConfig[] codecsSelectableSbcAacOpus;
        codecsSelectableSbcAacOpus = new BluetoothCodecConfig[3];
        codecsSelectableSbcAacOpus[0] = mCodecConfigSbc;
        codecsSelectableSbcAacOpus[1] = mCodecConfigAac;
        codecsSelectableSbcAacOpus[2] = mCodecConfigOpus;

        BluetoothCodecStatus codecStatusSbcAndSbc =
                new BluetoothCodecStatus(
                        mCodecConfigSbc,
                        Arrays.asList(codecsSelectableSbcAac),
                        Arrays.asList(codecsSelectableSbc));
        BluetoothCodecStatus codecStatusSbcAndSbcAac =
                new BluetoothCodecStatus(
                        mCodecConfigSbc,
                        Arrays.asList(codecsSelectableSbcAac),
                        Arrays.asList(codecsSelectableSbcAac));
        BluetoothCodecStatus codecStatusAacAndSbcAac =
                new BluetoothCodecStatus(
                        mCodecConfigAac,
                        Arrays.asList(codecsSelectableSbcAac),
                        Arrays.asList(codecsSelectableSbcAac));
        BluetoothCodecStatus codecStatusOpusAndSbcAacOpus =
                new BluetoothCodecStatus(
                        mCodecConfigOpus,
                        Arrays.asList(codecsSelectableSbcAacOpus),
                        Arrays.asList(codecsSelectableSbcAacOpus));

        // Set default codec status when device disconnected
        // Selected codec = SBC, selectable codec = SBC
        mA2dpStateMachine.processCodecConfigEvent(codecStatusSbcAndSbc);
        verify(mA2dpService).codecConfigUpdated(mTestDevice, codecStatusSbcAndSbc, false);
        verify(mA2dpService, times(1)).updateLowLatencyAudioSupport(mTestDevice);

        // Inject an event to change state machine to connected state
        A2dpStackEvent connStCh =
                new A2dpStackEvent(A2dpStackEvent.EVENT_TYPE_CONNECTION_STATE_CHANGED);
        connStCh.device = mTestDevice;
        connStCh.valueInt = A2dpStackEvent.CONNECTION_STATE_CONNECTED;
        mA2dpStateMachine.sendMessage(A2dpStateMachine.STACK_EVENT, connStCh);

        // Verify that the expected number of broadcasts are executed:
        // - two calls to broadcastConnectionState(): Disconnected -> Conecting -> Connected
        // - one call to broadcastAudioState() when entering Connected state
        ArgumentCaptor<Intent> intentArgument2 = ArgumentCaptor.forClass(Intent.class);
        verify(mA2dpService, timeout(TIMEOUT_MS).times(2))
                .sendBroadcast(intentArgument2.capture(), anyString(), any(Bundle.class));

        // Verify that state machine update optional codec when enter connected state
        verify(mA2dpService, times(1)).updateOptionalCodecsSupport(mTestDevice);
        verify(mA2dpService, times(2)).updateLowLatencyAudioSupport(mTestDevice);

        // Change codec status when device connected.
        // Selected codec = SBC, selectable codec = SBC+AAC
        mA2dpStateMachine.processCodecConfigEvent(codecStatusSbcAndSbcAac);
        if (!offloadEnabled) {
            verify(mA2dpService).codecConfigUpdated(mTestDevice, codecStatusSbcAndSbcAac, true);
        }
        verify(mA2dpService, times(2)).updateOptionalCodecsSupport(mTestDevice);
        verify(mA2dpService, times(3)).updateLowLatencyAudioSupport(mTestDevice);

        // Update selected codec with selectable codec unchanged.
        // Selected codec = AAC, selectable codec = SBC+AAC
        mA2dpStateMachine.processCodecConfigEvent(codecStatusAacAndSbcAac);
        verify(mA2dpService).codecConfigUpdated(mTestDevice, codecStatusAacAndSbcAac, false);
        verify(mA2dpService, times(2)).updateOptionalCodecsSupport(mTestDevice);
        verify(mA2dpService, times(4)).updateLowLatencyAudioSupport(mTestDevice);

        // Update selected codec
        // Selected codec = OPUS, selectable codec = SBC+AAC+OPUS
        mA2dpStateMachine.processCodecConfigEvent(codecStatusOpusAndSbcAacOpus);
        if (!offloadEnabled) {
            verify(mA2dpService)
                    .codecConfigUpdated(mTestDevice, codecStatusOpusAndSbcAacOpus, true);
        }
        verify(mA2dpService, times(3)).updateOptionalCodecsSupport(mTestDevice);
        // Check if low latency audio been updated.
        verify(mA2dpService, times(5)).updateLowLatencyAudioSupport(mTestDevice);

        // Update selected codec with selectable codec changed.
        // Selected codec = SBC, selectable codec = SBC+AAC
        mA2dpStateMachine.processCodecConfigEvent(codecStatusSbcAndSbcAac);
        if (!offloadEnabled) {
            verify(mA2dpService).codecConfigUpdated(mTestDevice, codecStatusSbcAndSbcAac, true);
        }
        // Check if low latency audio been update.
        verify(mA2dpService, times(6)).updateLowLatencyAudioSupport(mTestDevice);
    }

    @Test
    public void dump_doesNotCrash() {
        BluetoothCodecConfig[] codecsSelectableSbc;
        codecsSelectableSbc = new BluetoothCodecConfig[1];
        codecsSelectableSbc[0] = mCodecConfigSbc;

        BluetoothCodecConfig[] codecsSelectableSbcAac;
        codecsSelectableSbcAac = new BluetoothCodecConfig[2];
        codecsSelectableSbcAac[0] = mCodecConfigSbc;
        codecsSelectableSbcAac[1] = mCodecConfigAac;

        BluetoothCodecStatus codecStatusSbcAndSbc =
                new BluetoothCodecStatus(
                        mCodecConfigSbc,
                        Arrays.asList(codecsSelectableSbcAac),
                        Arrays.asList(codecsSelectableSbc));
        mA2dpStateMachine.processCodecConfigEvent(codecStatusSbcAndSbc);

        mA2dpStateMachine.dump(new StringBuilder());
    }
}
