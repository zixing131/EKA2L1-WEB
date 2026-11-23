# Tilt steering was dead in Ferrari GT but fine in Asphalt 6

On device, Ferrari GT (rm-707) ignored the accelerometer completely once a race
started. Asphalt 6, the other tilt-steered title on the same ROM, worked. Since both
talk to `!SensorServer` through `sensrvclient.dll`, the difference had to be in what
they ask for.

## What the guest actually asked for

A probe on the sensor server, run on hardware (the simulator has no CoreMotion, so
`queries_active_sensor` returns nothing there and the game never gets past the query),
recorded the whole exchange:

```
query type 0x0 datatype 0x0 quantity 0 context 0   ->  1 channels
open_channel 1
start_listening 1 desired 16 max 16 period 50
channel_data ch 1 (#1) max_bytes 96
```

and then nothing. One data request, never repeated — the game's active object was
still waiting when the race started.

The two numbers that matter are `desired 16` and `max_bytes 96`. A
`sensor_accelerometer_axis_data` is 24 bytes, so 96 bytes is room for exactly **four**
items while the game asked the server to buffer **sixteen**.

## Where the 4 comes from

`open_channel` reports a maximum buffer count to the client, and EKA2L1 has always
answered a hardcoded 4 ("Should always be like this hopefully"). The official client
sizes its receive buffer from that number and silently caps the request —
`CSensrvDataHandler::StartListeningL` in
`sensorservices/sensorserver/src/client/sensrvchannelimpl.cpp`:

```cpp
TInt size = iListeningParameters.iMaximumBufferingCount * iChannelInfo.iDataItemSize;
if( size <= 0 || ... || iListeningParameters.iMaximumBufferingCount > aMaxBufferCount )
    {
    size = aMaxBufferCount * iChannelInfo.iDataItemSize;
    }
```

So the client asks for 16, budgets for 4, and expects the server to respect the number
the server itself advertised. EKA2L1 passed the raw 16 to the sensor backend, which
then waited for sixteen samples before reporting anything. At the 40 Hz the iOS
backend runs, that is a 400 ms batch, of which only the **oldest four** samples fit in
the client's buffer — the twelve newest were dropped and reported as `lost_count`. The
game stopped re-arming after that first reply.

Asphalt 6 escapes this because it asks for a count that fits.

## Fix

`start_listening` now clamps both the desired and the maximum buffering count to the
same constant `open_channel` reports. On device the data requests immediately started
cycling (`#1`, `#51`, …) instead of stopping after the first one, and every batch of
four now arrives whole, every 100 ms.

Worth noting for later: the iOS backend still ignores `buffering_period` (the "report
whatever you have after this many ms" half of the contract) and
`get_all_properties` is unimplemented in every backend. Neither mattered here, but
they are the next things to suspect if another title's sensor use looks stuck.

A later review checked the remaining count boundaries against
`CSensrvChannelListener::StartListening`: a zero or oversized desired count selects
the advertised maximum; a maximum count below the normalized desired count or above
the advertised limit also selects the advertised maximum. The bridge now follows
those rules before calling either sensor backend, with unit coverage for zero,
reversed, oversized and in-range counts. This also avoids backend-specific treatment
of a valid zero-count request.
