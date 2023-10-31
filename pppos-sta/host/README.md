
# ESP-MQTT demo with PPPoS connectivity

This is a basic demo of using esp-mqtt library, but connects to the internet using a PPPoS client. To run this example, you would need a PPP server that provides connectivity to the MQTT broker used in this example (by default a public broker accessible on the internet).

The PPP server could be a Linux computer with `pppd` service or an ESP32 acting like a connection gateway with PPPoS server (see the "slave" project).
