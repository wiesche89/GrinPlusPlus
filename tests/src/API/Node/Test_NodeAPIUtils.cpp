#include <catch.hpp>

#include <API/Node/Handlers/NodeAPIUtils.h>

TEST_CASE("Node v2 owner peer params accept SocketAddr strings")
{
	Json::Value peer("70.50.33.130:3414");

	const SocketAddress address = NodeAPI::ParseSocketAddrParam(peer);

	REQUIRE(address.GetIPAddress().Format() == "70.50.33.130");
	REQUIRE(address.GetPortNumber() == 3414);
	REQUIRE(address.Format() == "70.50.33.130:3414");
}

TEST_CASE("Node v2 output printable uses Rust-compatible proof option shape")
{
	const Commitment commitment = Commitment::FromHex("09bab2bdba2e6aed690b5eda11accc13c06723ca5965bb460c5f2383655989af3f");
	RangeProof proof(std::vector<unsigned char>{ 0x01, 0x02, 0x03 });

	Json::Value withProof = NodeAPI::BuildOutputPrintable(
		EOutputFeatures::DEFAULT,
		commitment,
		proof,
		false,
		376150,
		4107711,
		true);

	REQUIRE(withProof["output_type"].asString() == "Transaction");
	REQUIRE(withProof["commit"].asString() == commitment.ToHex());
	REQUIRE(withProof["spent"].asBool() == false);
	REQUIRE(withProof["proof"].isString());
	REQUIRE(withProof["merkle_proof"].isNull());
	REQUIRE(withProof["block_height"].asUInt64() == 376150);
	REQUIRE(withProof["mmr_index"].asUInt64() == 4107711);

	Json::Value withoutProof = NodeAPI::BuildOutputPrintable(
		EOutputFeatures::DEFAULT,
		commitment,
		proof,
		false,
		376150,
		4107711,
		false);

	REQUIRE(withoutProof["proof"].isNull());
	REQUIRE(withoutProof["proof_hash"].isString());
}
