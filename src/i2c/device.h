#pragma once

#include <cstdint>
#include <string>

namespace bk {

// A slave on an I2C bus. The controller calls begin() after the address byte,
// then write() or read() once per data byte, then stop() when the transaction
// ends.
class I2cDevice {
  public:
    explicit I2cDevice(uint8_t address) : address_(address) {}
    virtual ~I2cDevice() = default;

    // The type name used on the command line.
    virtual std::string type() const = 0;
    uint8_t             address() const { return address_; }

    // The address byte matched this device; true acknowledges it.
    virtual bool begin(bool read) {
        (void)read;
        return true;
    }
    // A byte from the master; true acknowledges it.
    virtual bool write(uint8_t byte) {
        (void)byte;
        return true;
    }
    // The next byte for the master.
    virtual uint8_t read() { return 0xFF; }
    // The transaction ended.
    virtual void stop() {}

  private:
    uint8_t address_;
};

} // namespace bk
