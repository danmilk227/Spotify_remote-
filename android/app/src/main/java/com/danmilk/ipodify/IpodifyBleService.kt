package com.danmilk.ipodify

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
import android.os.ParcelUuid
import android.util.Log
import androidx.core.app.NotificationCompat
import java.util.UUID
import kotlin.concurrent.thread

class IpodifyBleService : Service() {

    companion object {
        private const val TAG = "IpodifyBleService"
        private const val NOTIF_CHANNEL_ID = "ipodify_channel"
        private const val NOTIF_ID = 1001

        val SERVICE_UUID: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
        val RX_UUID: UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E") // Телефон -> ESP32
        val TX_UUID: UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E") // ESP32 -> Телефон
        val OTA_UUID: UUID = UUID.fromString("6E400004-B5A3-F393-E0A9-E50E24DCCA9E") // OTA Binary Stream
        val CLIENT_CONFIG_DESCRIPTOR: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        var instance: IpodifyBleService? = null
        var isConnected: Boolean = false
        var currentStatus: String = "Отключено"
    }

    private val bluetoothAdapter: BluetoothAdapter? by lazy {
        val manager = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        manager.adapter
    }

    private var bluetoothGatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var otaChar: BluetoothGattCharacteristic? = null
    private var isScanning = false
    private val handler = Handler(Looper.getMainLooper())

    var isOtaInProgress = false
    var otaCallback: ((percent: Int, status: String) -> Unit)? = null
    var otaCompletion: ((success: Boolean, message: String) -> Unit)? = null

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

    fun updateNotification(statusText: String) {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        manager.notify(NOTIF_ID, buildNotification(statusText))
        MainActivity.instance?.updateUI()
    }

    fun startAutoConnectScan() {
        if (isConnected || isScanning) return
        val scanner = bluetoothAdapter?.bluetoothLeScanner
        if (scanner == null) {
            updateNotification("Bluetooth выключен")
            return
        }

        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(SERVICE_UUID))
            .build()

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        isScanning = true
        updateNotification("Поиск iPodify 3G...")

        try {
            scanner.startScan(listOf(filter), settings, scanCallback)
            // Таймаут на непрерывное сканирование — 15 секунд
            handler.postDelayed({
                if (isScanning && !isConnected) {
                    stopScan()
                    handler.postDelayed({ startAutoConnectScan() }, 3000)
                }
            }, 15000)
        } catch (e: Exception) {
            Log.e(TAG, "Start scan error: ${e.message}")
            // Fallback без фильтра UUID
            try {
                scanner.startScan(scanCallback)
            } catch (e2: Exception) {
                Log.e(TAG, "Unfiltered scan error: ${e2.message}")
            }
        }
    }

    private fun stopScan() {
        try {
            val scanner = bluetoothAdapter?.bluetoothLeScanner
            scanner?.stopScan(scanCallback)
        } catch (e: Exception) {
            Log.e(TAG, "Stop scan error: ${e.message}")
        }
        isScanning = false
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult?) {
            val device = result?.device ?: return
            val name = device.name ?: result.scanRecord?.deviceName
            val hasUuid = result.scanRecord?.serviceUuids?.contains(ParcelUuid(SERVICE_UUID)) == true

            if (name == "iPodify 3G" || (name != null && name.contains("iPodify")) || hasUuid) {
                Log.i(TAG, "Found target device: $name (${device.address})")
                stopScan()
                connectToDevice(device)
            }
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
        otaChar = null
        isConnected = false
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt?, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.i(TAG, "Connected to GATT server. Requesting MTU 512 & HIGH PRIORITY...")
                isConnected = true
                updateNotification("Подключено к iPodify 3G")
                gatt?.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                val mtuOk = gatt?.requestMtu(512) ?: false
                if (!mtuOk) {
                    gatt?.discoverServices()
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.w(TAG, "Disconnected from GATT server. Will auto-reconnect...")
                isConnected = false
                if (isOtaInProgress) {
                    isOtaInProgress = false
                    otaCompletion?.invoke(false, "Соединение разорвано во время обновления")
                }
                disconnectGatt()
                updateNotification("Отключено. Авто-повтор через 3 сек...")
                handler.postDelayed({ startAutoConnectScan() }, 3000)
            }
        }

        override fun onMtuChanged(gatt: BluetoothGatt?, mtu: Int, status: Int) {
            Log.i(TAG, "MTU changed to: $mtu. Discovering services...")
            gatt?.discoverServices()
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt?, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                val service = gatt?.getService(SERVICE_UUID)
                if (service != null) {
                    rxChar = service.getCharacteristic(RX_UUID)
                    otaChar = service.getCharacteristic(OTA_UUID)
                    val txChar = service.getCharacteristic(TX_UUID)

                    if (txChar != null) {
                        gatt.setCharacteristicNotification(txChar, true)
                        val descriptor = txChar.getDescriptor(CLIENT_CONFIG_DESCRIPTOR)
                        descriptor?.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        descriptor?.let { gatt.writeDescriptor(it) }
                        Log.i(TAG, "Subscribed to TX notifications!")
                        gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                    }

                    // Request initial state from media listener
                    MediaListenerService.instance?.updateActiveController()
                    MediaListenerService.instance?.resetArtCacheAndResend()
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

            if (cmd.startsWith("OTA_ACK:DONE")) {
                isOtaInProgress = false
                otaCompletion?.invoke(true, "Успешно! ESP32 перезагружается...")
                return
            } else if (cmd.startsWith("OTA_ERR:")) {
                isOtaInProgress = false
                otaCompletion?.invoke(false, "Ошибка ESP32: $cmd")
                return
            }

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

    fun startOtaUpdate(
        firmwareBytes: ByteArray,
        onProgress: (percent: Int, status: String) -> Unit,
        onComplete: (success: Boolean, message: String) -> Unit
    ) {
        val gatt = bluetoothGatt
        val char = otaChar
        if (!isConnected || gatt == null || char == null) {
            onComplete(false, "ESP32 не подключен по BLE или нет OTA канала")
            return
        }
        if (isOtaInProgress) {
            onComplete(false, "Обновление уже выполняется")
            return
        }

        isOtaInProgress = true
        otaCallback = onProgress
        otaCompletion = onComplete

        thread {
            try {
                onProgress(0, "Инициализация OTA на ESP32...")
                sendPacketToBle("OTA_START:${firmwareBytes.size}")
                Thread.sleep(400)

                val chunkSize = 128
                val totalSize = firmwareBytes.size
                var offset = 0
                var packetIndex = 0

                while (offset < totalSize && isOtaInProgress) {
                    val end = (offset + chunkSize).coerceAtMost(totalSize)
                    val chunk = firmwareBytes.copyOfRange(offset, end)

                    char.value = chunk
                    char.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                    
                    var success = gatt.writeCharacteristic(char)
                    var retries = 0
                    while (!success && retries < 25 && isOtaInProgress) {
                        Thread.sleep(15)
                        success = gatt.writeCharacteristic(char)
                        retries++
                    }

                    if (!success && isOtaInProgress) {
                        throw IllegalStateException("Сбой отправки пакета OTA на смещении $offset (буфер BLE переполнен)")
                    }

                    offset = end
                    packetIndex++
                    val pct = (offset * 100) / totalSize
                    onProgress(pct, "Отправка: $pct% ($offset / $totalSize B)")

                    // Небольшая пауза для гарантированной записи во Flash память ESP32
                    Thread.sleep(18)
                    if (packetIndex % 16 == 0) {
                        Thread.sleep(40)
                    }
                }

                if (isOtaInProgress) {
                    onProgress(100, "Проверка целостности и финализация...")
                    Thread.sleep(300)
                    sendPacketToBle("OTA_END:$totalSize")
                }
            } catch (e: Exception) {
                Log.e(TAG, "OTA failed: ${e.message}")
                isOtaInProgress = false
                sendPacketToBle("OTA_ABORT")
                onComplete(false, "Ошибка отправки: ${e.message}")
            }
        }
    }
}
