// One peer whose frames stay at the transmit FIFO head: everySize holds all sizes,
// otherwise only payload-sized (>=1200 B) frames. Shared by the awdl0 transmit ring
// (ItlAWDLInterface::popTransmit) and the coex pump that builds the hold list.
#pragma once
struct ItlAWDLTransmitHold { const uint8_t *mac; bool everySize; };
