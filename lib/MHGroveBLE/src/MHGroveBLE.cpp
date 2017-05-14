/*
MIT License

Copyright (c) 2017 Marc Haisenko

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "MHGroveBLE.h"

/*
Some notes about the Seeed Grove BLE:

The commands and responses are sent and received WITHOUT any delimiters like
"\r\n". After sending the command, you just wait for the response. You know the
reponse is complete if you don't receive any more stuff so you have to wait for
a timeout.

How utterly stupid! But that's the way it is.
*/

static const unsigned long kDefaultTimeout = 500;
static const unsigned long kRetryTimeout = 500;
static const unsigned long kWaitForDeviceTimeout = 5000;

/** Internal state. */
enum class MHGroveBLE::InternalState {
  /** Initial state. */
  startup,
  /** Send "AT" periodically and wait until the device responds. */
  waitForDeviceAfterStartup,
  /** Set the Bluetooth name. */
  setName,
  /** Set the device role. */
  setRole,
  /** Set that we want to be notified about connections. */
  setNotification,
  /** Reset after setting the device up. */
  reset,
  /** Send "AT" periodically and wait until the device responds. */
  waitForDeviceAfterReset,

  /** An unrecoverable error occurred, operation is halted. */
  panicked,

  /** Waiting for a connection. */
  waitingForConnection,
  /** A peer has connected. */
  connected
};

/** Return value for `receiveResponse`. */
enum class MHGroveBLE::ResponseState {
  /** Still receiving data. */
  receiving,
  /** Nothing received so far, need to resend the command. */
  needRetry,
  /** The timeout has been reached, no data received. */
  timedOut,
  /** The retry or timeout time has been reached, we have data in `rxBuffer`. */
  success
};


MHGroveBLE::MHGroveBLE(
  Stream & device,
  const char * name,
  unsigned int rxBufferSize
) :
  device(device),
  name(name),
  internalState(InternalState::startup)
{
  rxBuffer.reserve(rxBufferSize);
}

void MHGroveBLE::runOnce()
{
  switch (internalState) {
    case InternalState::startup:
      transitionToState(InternalState::waitForDeviceAfterStartup);
      break;

    case InternalState::waitForDeviceAfterStartup:
    case InternalState::waitForDeviceAfterReset:
      handleWaitForDevice();
      break;

    case InternalState::setName:
    case InternalState::setRole:
    case InternalState::setNotification:
      handleGenericCommand();
      break;

    case InternalState::reset:
      handleReset();
      break;

    case InternalState::waitingForConnection:
      break;

    case InternalState::connected:
      break;

    case InternalState::panicked:
      // Once we've panicked we won't do anything again.
      break;
  }
}

MHGroveBLE::State MHGroveBLE::getState() {
  switch (internalState) {
    case InternalState::panicked:   return State::panicked;
    case InternalState::waitingForConnection: return State::waitingForConnection;
    case InternalState::connected:  return State::connected;
    default:                        return State::initializing;
  }
}

void MHGroveBLE::setDebug(void (*debugFunc)(const char *))
{
  debug = debugFunc;
}

void MHGroveBLE::sendCommand(const char * command)
{
  if (debug) {
    String text = "Sending command: ";
    text += command;
    debug(text.c_str());
  }

  device.print(command);
  // device.print("\r\n");

  // Clear the receive buffer after sending a command.
  rxBuffer = "";
}

MHGroveBLE::ResponseState MHGroveBLE::receiveResponse()
{
  unsigned long now = millis();
  bool timeoutReached = (now >= timeoutTime);

  while (device.available() > 0) {
    int value = device.read();
    if (value < 0) {
      // Shouldn't happen? We asked whether there's stuff available!
      break;
    }
    rxBuffer += (char)value;
  }

  if ((retryTime > 0 && now >= retryTime) || timeoutReached) {
    if (rxBuffer.length() > 0) {
      if (debug) {
        String text = "Received response: ";
        text += rxBuffer;
        debug(text.c_str());
      }
      return ResponseState::success;

    } else if (timeoutReached) {
      return ResponseState::timedOut;

    } else {
      retryTime = millis() + kRetryTimeout;
      return ResponseState::needRetry;
    }

  } else {
    return ResponseState::receiving;
  }
}

void MHGroveBLE::transitionToState(MHGroveBLE::InternalState nextState)
{
  unsigned long now = millis();

  if (debug) {
    String text = "Transitioning to state: ";
    text += (int)nextState;
    debug(text.c_str());
  }

  switch (nextState) {
    case InternalState::startup: break;

    case InternalState::waitForDeviceAfterStartup:
      sendCommand("AT");
      retryTime = now + kRetryTimeout;
      timeoutTime = now + kWaitForDeviceTimeout;
      break;

    case InternalState::setName: {
      String command = "AT+NAME";
      command += name;
      sendCommand(command.c_str());
      retryTime = 0;
      timeoutTime = now + kDefaultTimeout;
      genericNextInternalState = InternalState::setRole;
      break;
    }

    case InternalState::setRole:
      sendCommand("AT+ROLE0");
      retryTime = 0;
      timeoutTime = now + kDefaultTimeout;
      genericNextInternalState = InternalState::setNotification;
      break;

    case InternalState::setNotification:
      sendCommand("AT+NOTI1");
      retryTime = 0;
      timeoutTime = now + kDefaultTimeout;
      genericNextInternalState = InternalState::reset;
      break;

    case InternalState::reset:
      sendCommand("AT+RESET");
      retryTime = 0;
      timeoutTime = now + kDefaultTimeout;
      break;

    case InternalState::waitForDeviceAfterReset:
      sendCommand("AT");
      retryTime = now + kRetryTimeout;
      timeoutTime = now + kWaitForDeviceTimeout;
      break;

    case InternalState::waitingForConnection:
      break;

    case InternalState::connected:
      break;

    case InternalState::panicked:
      if (debug) {
        debug("Panic!");
      }
      break;
  }

  internalState = nextState;
}

void MHGroveBLE::handleWaitForDevice()
{
  switch (receiveResponse()) {
    case ResponseState::receiving:
      break;

    case ResponseState::needRetry:
      sendCommand("AT");
      break;

    case ResponseState::timedOut:
      panic();
      break;

    case ResponseState::success:
      switch (internalState) {
        case InternalState::waitForDeviceAfterStartup:
          transitionToState(InternalState::setName);
          break;

        case InternalState::waitForDeviceAfterReset:
          transitionToState(InternalState::waitingForConnection);
          break;

        default:
          // Bug! Must not happen.
          panic();
      }
      break;
  }
}

void MHGroveBLE::handleGenericCommand()
{
  switch (receiveResponse()) {
    case ResponseState::receiving:
      break;

    case ResponseState::needRetry: // Bug, must not happen
    case ResponseState::timedOut:
      panic();
      break;

    case ResponseState::success:
      transitionToState(genericNextInternalState);
      break;
  }
}

void MHGroveBLE::handleReset()
{
  switch (receiveResponse()) {
    case ResponseState::receiving:
      break;

    case ResponseState::needRetry: // Bug, must not happen
      panic();
      break;

    case ResponseState::timedOut:
      // Try waiting for the device.
      // Fall-through
    case ResponseState::success:
      transitionToState(InternalState::waitForDeviceAfterReset);
      break;
  }
}

void MHGroveBLE::panic()
{
  transitionToState(InternalState::panicked);
}
