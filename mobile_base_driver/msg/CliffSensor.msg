# Provides a cliff sensor reading
# This message is generated whenever a particular cliff sensor signals that the
# robot approaches or moves away from a cliff.

# cliff sensor
uint8 RIGHT     = 0
uint8 MIDRIGHT  = 1
uint8 MIDLEFT   = 2
uint8 LEFT      = 3
uint8 BACKLEFT  = 4
uint8 BACKRIGHT = 5

# cliff sensor state
uint8 FLOOR = 0
uint8 CLIFF = 1

uint8 sensor
uint8 state

# distance to floor when cliff was detected
float32 distance

# return rate from the vl6180x, rear cliff sensors only
float32 return_rate
