# App Scanner BLE

Esta app está diseñada para correr en un celular dentro de los vehículos.

Se conecta por MQTT a un servidor central para enviar los datos sobre la posición del vehículo y los beacons BLE que encuentra en su cercanía.

# Configuración

La aplicación toma la configuración desde un archivo llamado ``scanner_settings.json`` ubicado en la carpeta de Descargas.

Ejemplo de configuración:

```json
{
  "location_id":"ABC 1234",
  "mqtt_server_url":"tcp://172.30.23.23:1883",
  "mqtt_user":"mobile-scanner",
  "mqtt_pass":"mobile-passwd"
}
```

Los campos ``mqtt_user`` y ``mqtt_pass`` pueden ser omitidos, en ese caso se usan los valores por defecto para establecer la conexión con el broker.
Es posible utilizar las mismas credenciales en múltiples clientes.

Luego de instalada hay que ir a la configuración del celular para darle los permisos de archivos y ubicación.
