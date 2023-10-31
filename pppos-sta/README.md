# Dual chip solution with PPPoS + NAT

```
             SLAVE micro                                  HOST micro
    \|/  +----------------+                           +----------------+
     |   |                |          serial line      |                |
     +---+ WiFi NAT PPPoS |===== UART / USB / SPI ====| PPPoS client   |
         |        (server)|                           |                |
         +----------------+                           +----------------+
```
