#pragma once

#include <Common/Secure.h>
#include <optional>
#include <vector>
#include <stdint.h> 

class Mnemonic
{
public:
	static SecureString CreateMnemonic(const uint8_t* entropy, const size_t entropy_len);
	static SecureVector ToEntropy(const SecureString& walletWords);
};