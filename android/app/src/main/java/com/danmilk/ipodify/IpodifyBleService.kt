package com.danmilk.ipodify

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import androidx.core.app.NotificationCompat
import java.util.UUID

@SuppressLint("MissingPermission")
class IpodifyBleService : Service() {

    companion object {
        private const val TAG = "iPodifyBLE"
        private const val NOTIF_CHANNEL_ID = "ipodify_ble_channel"
        private const val NOTIF_ID = 1001

        val SERVICE_UUID: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
        val RX_UUID: UUID      = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E") // Телефон -> ESP32
        val TX_UUID: UUID      = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E") // ESP32 -> Телефон
        val CLIENT_CONFIG_DESCRIPTOR: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        var instance: IpodifyBleService? = null
        var isConnected: Boolean = false
        var currentStatus: String = "Инициализация..."
    }

    private val bluetoothManager by lazy { getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager }
    private val bluetoothAdapter: BluetoothAdapter? get() = bluetoothManager.adapter
    private var bluetoothGatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null

    private var isScanning = false
    private val handler = Handler(Looper.getMainLooper())

    override fun onCreate() {
        super.onCreate()
        instance = this
        createNotificationChannel()
        startForeground(NOTIF_ID, buildNotification("Поиск iPodify 3G..."))
        startAutoConnectScan()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (!isConnected && !isScanning) {
            startAutoConnectScan()
        }
        return START_STICKY
    }

    override fun onDestroy() {
        super.onDestroy()
        stopScan()
        disconnectGatt()
        instance = null
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                NOTIF_CHANNEL_ID,
                "iPodify Background Service",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Поддержание фонового подключения к пульту iPodify"
            }
            val manager = getSystemService(NotificationManager::class.java)
            manager?.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(statusText: String): Notification {
        currentStatus = statusText
        val launchIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP
        }
        val pendingIntent = PendingIntent.getActivity(
            this, 0, launchIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(this, NOTIF_CHANNEL_ID)
            .setContentTitle("iPodify 3G Remote")
            .setContentText(statusText)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .build()
    }

    private fun updateNotification(statusText: String) {
        val manager = getSystemService(NotificationManager::class.java)
        manager?.notify(NOTIF_ID, buildNotification(statusText))
        MainActivity.instance?.updateUI()
    }

    fun startAutoConnectScan() {
        val scanner = bluetoothAdapter?.bluetoothLeScanner ?: return
        if (isScanning || isConnected) return

        isScanning = true
        updateNotification("Поиск устройства iPodify...")
        Log.i(TAG, "Starting BLE scan for iPodify...")

        val filter = ScanFilter.Builder()
            .setDeviceName("iPodify 3G")
            .build()

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        scanner.startScan(listOf(filter), settings, scanCallback)
    }

    private fun stopScan() {
        if (!isScanning) return
        val scanner = bluetoothAdapter?.bluetoothLeScanner ?: return
        try {
            scanner.stopScan(scanCallback)
        } catch (e: Exception) {
            Log.e(TAG, "Stop scan error: ${e.message}")
        }
        isScanning = false
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult?) {
            val device = result?.device ?: return
            Log.i(TAG, "Found device: ${device.name} (${device.address})")
            stopScan()
            connectToDevice(device)
        }

        override fun onScanFailed(errorCode: Int) {
            Log.e(TAG, "Scan failed: $errorCode")
            isScanning = false
            handler.postDelayed({ startAutoConnectScan() }, 5000)
        }
    }

    private fun connectToDevice(device: BluetoothDevice) {
        updateNotification("Подключение к ${device.name}...")
        bluetoothGatt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            device.connectGatt(this, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } else {
            device.connectGatt(this, false, gattCallback)
        }
    }

    private fun disconnectGatt() {
        try {
            bluetoothGatt?.disconnect()
            bluetoothGatt?.close()
        } catch (e: Exception) {}
        bluetoothGatt = null
        rxChar = null
        isConnected = false
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt?, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.i(TAG, "Connected to GATT server. Discovering services...")
                isConnected = true
                updateNotification("Подключено к iPodify 3G")
                gatt?.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.w(TAG, "Disconnected from GATT server. Will auto-reconnect...")
                isConnected = false
                disconnectGatt()
                updateNotification("Отключено. Авто-повтор через 3 сек...")
                handler.postDelayed({ startAutoConnectScan() }, 3000)
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt?, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                val service = gatt?.getService(SERVICE_UUID)
                if (service != null) {
                    rxChar = service.getCharacteristic(RX_UUID)
                    val txChar = service.getCharacteristic(TX_UUID)

                    if (txChar != null) {
                        gatt.setCharacteristicNotification(txChar, true)
                        val descriptor = txChar.getDescriptor(CLIENT_CONFIG_DESCRIPTOR)
                        descriptor?.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        descriptor?.let { gatt.writeDescriptor(it) }
                        Log.i(TAG, "Subscribed to TX notifications!")
                    }

                    // Request initial state from media listener
                    MediaListenerService.instance?.updateActiveController()
                    MediaListenerService.instance?.broadcastCurrentPlayback()
                } else {
                    Log.e(TAG, "Nordic UART Service not found on device!")
                }
            }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt?,
            characteristic: BluetoothGattCharacteristic?
        ) {
            val data = characteristic?.value ?: return
            val cmd = String(data, Charsets.UTF_8).trim()
            Log.i(TAG, "Received command from ESP32: $cmd")
            MediaListenerService.instance?.handleCommand(cmd)
        }
    }

    fun sendPacketToBle(packet: String) {
        val gatt = bluetoothGatt ?: return
        val char = rxChar ?: return
        if (!isConnected) return

        try {
            char.value = packet.toByteArray(Charsets.UTF_8)
            char.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            gatt.writeCharacteristic(char)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to send packet to BLE: ${e.message}")
        }
    }
}
