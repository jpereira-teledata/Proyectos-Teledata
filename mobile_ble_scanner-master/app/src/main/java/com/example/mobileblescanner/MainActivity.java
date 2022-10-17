package com.example.mobileblescanner;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;

import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.location.Location;
import android.location.LocationManager;
import android.location.LocationListener;
import android.os.BatteryManager;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.util.Log;
import android.view.WindowManager;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.Toast;

import org.eclipse.paho.android.service.MqttAndroidClient;
import org.eclipse.paho.client.mqttv3.IMqttActionListener;
import org.eclipse.paho.client.mqttv3.IMqttDeliveryToken;
import org.eclipse.paho.client.mqttv3.IMqttToken;
import org.eclipse.paho.client.mqttv3.MqttCallback;
import org.eclipse.paho.client.mqttv3.MqttConnectOptions;
import org.eclipse.paho.client.mqttv3.MqttException;
import org.eclipse.paho.client.mqttv3.MqttMessage;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

public class MainActivity extends AppCompatActivity {

    private static final String TAG = "Mobile GPS Scanner";

    private static final String SETTINGS_FILENAME = "scanner_settings.json";

    String location = "Mobile GPS Scanner";
    String mqtt_server_url = "tcp://192.168.0.3:1883";
    String client_id = "mobile_ble_scanner";

    String mqtt_user = "mobile-scanner";
    String mqtt_pass = "mobile-passwd";

    MqttAndroidClient mqtt_client;

    private static final long MQTT_RECONNECT_INTERVAL = 10000;

    private BluetoothLeScanner bluetoothLeScanner;
    private boolean scanning;
    private Handler stopHandler;
    private Handler scanHandler;
    private Handler gpsIconHandler;

    private static final long SCAN_DURATION = 10000;
    private static final long SCAN_INTERVAL = 30000;

    private static final int PATH_LOSS = 4;

    private static final int LOCATION_INTERVAL = 60000;
    private static final int LOCATION_DISTANCE = 200;

    Set<String> beacons = new HashSet<>();
    int tag_counter;
    int people_counter;
    int devices_counter;
    int tools_counter;

    TextView tagTextView;
    TextView peopleTextView;
    TextView devicesTextView;
    TextView toolsTextView;

    private static final int PEOPLE_MINOR = 1;
    private static final int DEVICE_MINOR = 2;
    private static final int TOOL_MINOR = 3;

    ImageView mqttImageView;
    ImageView bleImageView;
    ImageView gpsImageView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        String state = Environment.getExternalStorageState();
        boolean isReadable = (Environment.MEDIA_MOUNTED.equals(state) ||
                Environment.MEDIA_MOUNTED_READ_ONLY.equals(state));

        if (isReadable) {
            File settings_file = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), SETTINGS_FILENAME);

            try {
                InputStream inputStream = new FileInputStream(settings_file);
                int size = inputStream.available();
                byte[] buffer = new byte[size];
                inputStream.read(buffer);
                inputStream.close();

                String json = new String(buffer, StandardCharsets.UTF_8);
                JSONObject obj = new JSONObject(json);

                if(obj.has("location_id") && obj.has("mqtt_server_url")) {
                    location = obj.getString("location_id");
                    client_id = location.toLowerCase(Locale.ROOT).replaceAll(" ", "_");
                    mqtt_server_url = obj.getString("mqtt_server_url");
                    if(obj.has("mqtt_user") && obj.has("mqtt_pass")) {
                        mqtt_user = obj.getString("mqtt_user");
                        mqtt_pass = obj.getString("mqtt_pass");
                    } else {
                        Log.w(TAG, "Using default user and password");
                    }
                } else {
                    Toast.makeText(getApplicationContext(), getString(R.string.bad_config_msg), Toast.LENGTH_LONG).show();
                }

            } catch (FileNotFoundException e) {
                Log.e(TAG, e.getMessage());
                Toast.makeText(getApplicationContext(), getString(R.string.not_found_msg), Toast.LENGTH_LONG).show();
            } catch (IOException e) {
                e.printStackTrace();
            } catch (JSONException e) {
                Log.e(TAG, e.getMessage());
                Toast.makeText(getApplicationContext(), getString(R.string.read_error_msg), Toast.LENGTH_LONG).show();
            }
        } else {
            Log.e(TAG, "The storage is not readable");
            Toast.makeText(getApplicationContext(), getString(R.string.not_found_msg), Toast.LENGTH_LONG).show();
        }

        ((TextView)findViewById(R.id.locationTextView)).setText(location);

        connectMQTT();

        stopHandler = new Handler();
        scanHandler = new Handler();
        gpsIconHandler = new Handler();

        mqttImageView = findViewById(R.id.mqttImageView);
        bleImageView = findViewById(R.id.bleImageView);
        gpsImageView = findViewById(R.id.gpsImageView);

        tagTextView = findViewById(R.id.tag_counter);
        peopleTextView = findViewById(R.id.people_counter);
        devicesTextView = findViewById(R.id.devices_counter);
        toolsTextView = findViewById(R.id.tools_counter);

        LocationManager locationManager = (LocationManager) getSystemService(Context.LOCATION_SERVICE);

        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED && ActivityCompat.checkSelfPermission(this, Manifest.permission.ACCESS_COARSE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
            Log.e(TAG, "Missing location permissions");
            return;
        }

        LocationListener locationListener = new MyLocationListener();
        locationManager.requestLocationUpdates(LocationManager.GPS_PROVIDER, LOCATION_INTERVAL, LOCATION_DISTANCE, locationListener);

        BluetoothManager btManager = (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
        BluetoothAdapter btAdapter = btManager.getAdapter();
        bluetoothLeScanner = btAdapter.getBluetoothLeScanner();

        if (bluetoothLeScanner != null && !btAdapter.isEnabled()) {
            Log.e(TAG, "Missing ble");
            return;
        }

        scanHandler.post(new Runnable() {
            @Override
            public void run() {
                scanLeDevices();
                scanHandler.postDelayed(this, SCAN_INTERVAL);
            }
        });
    }

    private void connectMQTT() {
        Log.d("Totem", "connectMQTT");

        if (mqtt_client == null) {
            mqtt_client = new MqttAndroidClient(MainActivity.this, mqtt_server_url, client_id);
            mqtt_client.setCallback(new MqttCallback() {
                @Override
                public void connectionLost(Throwable cause) {
                    Log.e(TAG, "connectMQTT connectionLost");

                    mqttImageView.setImageResource(R.drawable.ic_server_network_off_black_18dp);

                    // Retry connect
                    new android.os.Handler().postDelayed(
                            () -> connectMQTT(),
                            MQTT_RECONNECT_INTERVAL);
                }

                @Override
                public void messageArrived(String topic, MqttMessage message) {
                    String payload = new String(message.getPayload());
                    Log.d(TAG, "MQTT messageArrived " + payload);

                    TextView carTextView = findViewById(R.id.carTextView);
                    ImageView carImageView = findViewById(R.id.carImageView);

                    if (payload.equals("ON")){
                        carTextView.setText(R.string.car_on);
                        carImageView.setImageResource(R.drawable.ic_car_wrench_black_48dp);
                    } else {
                        carTextView.setText(R.string.car_off);
                        carImageView.setImageResource(R.drawable.ic_car_black_48dp);
                    }

                }

                @Override
                public void deliveryComplete(IMqttDeliveryToken token) {

                }
            });
        }

        MqttConnectOptions options = new MqttConnectOptions();
        options.setUserName(mqtt_user);
        options.setPassword(mqtt_pass.toCharArray());
        options.setCleanSession(true);
        options.setWill("homeassistant/binary_sensor/" + client_id + "/state", "OFF".getBytes(), 1, true);

        try {
            IMqttToken token = mqtt_client.connect(options);
            token.setActionCallback(new IMqttActionListener() {
                @Override
                public void onSuccess(IMqttToken asyncActionToken) {
                    Log.d(TAG, "connectMQTT onSuccess");

                    mqttImageView.setImageResource(R.drawable.ic_server_network_black_18dp);

                    byte[] encodedPayload;

                    // Switch
                    encodedPayload = ("{\"~\":\"homeassistant/switch/" + client_id + "\",\"name\":\"" + client_id + "\",\"cmd_t\":\"~/command\",\"retain\":true,\"icon\":\"mdi:car\"}").getBytes(StandardCharsets.UTF_8);
                    MqttMessage conf_message = new MqttMessage(encodedPayload);
                    conf_message.setRetained(true);

                    try {
                        mqtt_client.publish("homeassistant/switch/" + client_id + "/config", conf_message);
                    } catch (MqttException e) {
                        e.printStackTrace();
                    }

                    // Conectivity
                    encodedPayload = ("{\"~\":\"homeassistant/binary_sensor/" + client_id + "\",\"name\":\"" + client_id + "\",\"stat_t\":\"~/state\",\"json_attr_t\":\"~/attributes\",\"dev_cla\":\"connectivity\",\"icon\":\"mdi:cellphone-sound\",\"unique_id\":\"" + client_id + "\"}").getBytes(StandardCharsets.UTF_8);
                    conf_message = new MqttMessage(encodedPayload);
                    conf_message.setRetained(true);

                    try {
                        mqtt_client.publish("homeassistant/binary_sensor/" + client_id + "/config", conf_message);
                    } catch (MqttException e) {
                        e.printStackTrace();
                    }

                    encodedPayload = "ON".getBytes(StandardCharsets.UTF_8);
                    MqttMessage message = new MqttMessage(encodedPayload);
                    message.setRetained(true);

                    try {
                        mqtt_client.publish("homeassistant/binary_sensor/" + client_id + "/state", message);
                    } catch (MqttException e) {
                        e.printStackTrace();
                    }

                    // Charging
                    encodedPayload = ("{\"~\":\"homeassistant/binary_sensor/" + client_id + "_charging\",\"name\":\"" + client_id + "_charging\",\"stat_t\":\"~/state\",\"json_attr_t\":\"~/attributes\",\"dev_cla\":\"battery_charging\",\"unique_id\":\"" + client_id + "_charging\"}").getBytes(StandardCharsets.UTF_8);
                    conf_message = new MqttMessage(encodedPayload);
                    conf_message.setRetained(true);

                    try {
                        mqtt_client.publish("homeassistant/binary_sensor/" + client_id + "_charging/config", conf_message);
                    } catch (MqttException e) {
                        e.printStackTrace();
                    }

                    // Charge
                    encodedPayload = ("{\"~\":\"homeassistant/sensor/" + client_id + "_charge\",\"name\":\"" + client_id + "_charge\",\"stat_t\":\"~/state\",\"json_attr_t\":\"~/attributes\",\"dev_cla\":\"battery\",\"unit_of_measurement\":\"%\",\"state_class\":\"measurement\",\"unique_id\":\"" + client_id + "_charge\"}").getBytes(StandardCharsets.UTF_8);
                    conf_message = new MqttMessage(encodedPayload);
                    conf_message.setRetained(true);

                    try {
                        mqtt_client.publish("homeassistant/sensor/" + client_id + "_charge/config", conf_message);
                    } catch (MqttException e) {
                        e.printStackTrace();
                    }

                    try {
                        IMqttToken subToken = mqtt_client.subscribe("homeassistant/switch/" + client_id + "/command", 0);
                        subToken.setActionCallback(new IMqttActionListener() {
                            @Override
                            public void onSuccess(IMqttToken asyncActionToken) {
                                Log.d(TAG, "subscribeMQTT onSuccess");
                            }

                            @Override
                            public void onFailure(IMqttToken asyncActionToken, Throwable exception) {
                                Log.e(TAG, "subscribeMQTT onFailure");
                            }
                        });

                    } catch (MqttException e) {
                        e.printStackTrace();
                    }
                }

                @Override
                public void onFailure(IMqttToken asyncActionToken, Throwable exception) {
                    Log.e(TAG, "connectMQTT onFailure");
                    Log.e(TAG, asyncActionToken.getException().getMessage());

                    // Retry connect
                    new android.os.Handler().postDelayed(
                            () -> connectMQTT(),
                            MQTT_RECONNECT_INTERVAL);
                }
            });
        } catch (MqttException e) {
            Log.e(TAG, "connectMQTT error");
        }
    }

    private class MyLocationListener implements LocationListener {

        @Override
        public void onLocationChanged(Location loc) {
            gpsImageView.setImageResource(R.drawable.ic_crosshairs_gps_black_18dp);

            String pos = String.format(Locale.US,"latitude: %.10f, longitude: %.10f", loc.getLatitude(), loc.getLongitude());
            Log.v(TAG, pos);

            byte[] encodedPayload;

            encodedPayload = (String.format(Locale.US,"{\"location\":\"" + location + "\",\"latitude\":\"%.10f\",\"longitude\":\"%.10f\",\"source_type\":\"gps\"}", loc.getLatitude(), loc.getLongitude())).getBytes(StandardCharsets.UTF_8);
            MqttMessage message = new MqttMessage(encodedPayload);

            try {
                if(mqtt_client.isConnected()) {
                    mqtt_client.publish("homeassistant/binary_sensor/" + client_id + "/attributes", message);
                }
            } catch (MqttException e) {
                e.printStackTrace();
            }

            gpsIconHandler.postDelayed(() -> gpsImageView.setImageResource(R.drawable.ic_crosshairs_black_18dp), 10000);
        }

        @Override
        public void onProviderDisabled(String provider) {}

        @Override
        public void onProviderEnabled(String provider) {}

        @Override
        public void onStatusChanged(String provider, int status, Bundle extras) {}
    }

    public static String byteArrayToHexString(final byte[] bytes) {
        StringBuilder sb = new StringBuilder();
        for(byte b : bytes){
            sb.append(String.format("%02x", b&0xff));
        }
        return sb.toString();
    }

    private void findBeaconPattern(byte[] scanRecord, int rssi) {
        int startByte = 2;
        boolean patternFound = false;
        while (startByte <= 5) {
            if (((int) scanRecord[startByte + 2] & 0xff) == 0x02 && //Identifies an iBeacon
                    ((int) scanRecord[startByte + 3] & 0xff) == 0x15) { //Identifies correct data length
                patternFound = true;
                break;
            }
            startByte++;
        }

        if (patternFound) {
            // uuid
            byte[] uuidBytes = new byte[16];
            System.arraycopy(scanRecord, startByte + 4, uuidBytes, 0, 16);
            String hexString = byteArrayToHexString(uuidBytes);

            // major
            final int major = (scanRecord[startByte + 20] & 0xff) * 0x100 + (scanRecord[startByte + 21] & 0xff);

            // minor
            final int minor = (scanRecord[startByte + 22] & 0xff) * 0x100 + (scanRecord[startByte + 23] & 0xff);

            hexString = hexString + "-" + major + "-" + minor;

            if (beacons.add(hexString)) {

                switch (minor){
                    case PEOPLE_MINOR:
                        people_counter++;
                        break;
                    case DEVICE_MINOR:
                        devices_counter++;
                        break;
                    case TOOL_MINOR:
                        tools_counter++;
                        break;
                    default:
                        tag_counter++;
                        break;
                }

                int txCalibratedPower = scanRecord[startByte + 24];

                double ratio_db = txCalibratedPower - rssi;
                double ratio_linear = Math.pow(10, ratio_db / (10 * PATH_LOSS));

                Log.i(TAG, hexString + ", tx_power: " + txCalibratedPower + ", rssi: " + rssi);

                byte[] encodedPayload = (String.format(Locale.US, "{\"scanner_id\":\"" + client_id + "\",\"uuid\":\"" + hexString + "\",\"distance\":%.1f}", ratio_linear)).getBytes(StandardCharsets.UTF_8);

                MqttMessage message = new MqttMessage(encodedPayload);

                try {
                    if(mqtt_client.isConnected()) {
                        mqtt_client.publish("scan_results", message);
                    }
                } catch (MqttException e) {
                    e.printStackTrace();
                }
            } else {
                Log.i(TAG, hexString + " already exists");
            }
        }
    }

    private final ScanCallback leScanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            byte[] scanRecord = result.getScanRecord().getBytes();
            int rssi = result.getRssi();
            findBeaconPattern(scanRecord, rssi);
        }

        @Override
        public void onScanFailed(int errorCode) {
            Log.e(TAG, "Scan failed err " + errorCode);
        }
    };

    private void scanLeDevices() {
        if (!scanning) {
            stopHandler.postDelayed(() -> {
                scanning = false;
                bluetoothLeScanner.stopScan(leScanCallback);
                bleImageView.setImageResource(R.drawable.ic_tag_outline_black_18dp);

                tagTextView.setText(String.format(Locale.US, "%d", tag_counter));
                peopleTextView.setText(String.format(Locale.US, "%d", people_counter));
                devicesTextView.setText(String.format(Locale.US, "%d", devices_counter));
                toolsTextView.setText(String.format(Locale.US, "%d", tools_counter));
            }, SCAN_DURATION);

            beacons.clear();

            tag_counter = 0;
            people_counter = 0;
            devices_counter = 0;
            tools_counter = 0;

            scanning = true;
            bluetoothLeScanner.startScan(leScanCallback);
            bleImageView.setImageResource(R.drawable.ic_tag_black_18dp);

            // Update battery sensors
            IntentFilter ifilter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
            Intent batteryStatus = getApplicationContext().registerReceiver(null, ifilter);

            // Are we charging / charged?
            int status = batteryStatus.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
            boolean isCharging = status == BatteryManager.BATTERY_STATUS_CHARGING;

            byte[] encodedPayload;

            if(isCharging){
                Log.i(TAG, "Charging");
                encodedPayload = "ON".getBytes(StandardCharsets.UTF_8);
            } else {
                Log.i(TAG, "Not charging");
                encodedPayload = "OFF".getBytes(StandardCharsets.UTF_8);
            }

            MqttMessage message = new MqttMessage(encodedPayload);

            try {
                if(mqtt_client.isConnected()){
                    mqtt_client.publish("homeassistant/binary_sensor/" + client_id + "_charging/state", message);
                }
            } catch (MqttException e) {
                e.printStackTrace();
            }

            int level = batteryStatus.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
            int scale = batteryStatus.getIntExtra(BatteryManager.EXTRA_SCALE, -1);

            int batteryPct = level * 100 / scale;

            Log.i(TAG, "Percentage " + batteryPct + "%");

            encodedPayload = Integer.toString(batteryPct).getBytes(StandardCharsets.UTF_8);
            message = new MqttMessage(encodedPayload);

            try {
                if(mqtt_client.isConnected()) {
                    mqtt_client.publish("homeassistant/sensor/" + client_id + "_charge/state", message);
                }
            } catch (MqttException e) {
                e.printStackTrace();
            }
        } else {
            scanning = false;
            bluetoothLeScanner.stopScan(leScanCallback);
        }
    }
}