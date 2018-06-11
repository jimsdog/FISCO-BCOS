/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file Eth.cpp
 * @authors:
 *   Gav Wood <i@gavwood.com>
 *   Marek Kotewicz <marek@ethdev.com>
 * @date 2014
 */

#include <csignal>
#include <jsonrpccpp/common/exception.h>
#include <libdevcore/CommonData.h>
#include <libevmcore/Instruction.h>
#include <libethereum/Client.h>
#include <libethereum/Pool.hpp>
#include <libethereum/BlockQueue.h>
#include <libpbftseal/PBFT.h>
#include <libwebthree/WebThree.h>
#include <libethcore/CommonJS.h>
#include <libweb3jsonrpc/JsonHelper.h>
#include <libdevcore/easylog.h>
#include <abi/SolidityExp.h>
#include <abi/SolidityCoder.h>
#include <abi/ContractAbiMgr.h>
#include <libweb3jsonrpc/JsonHelper.h>

#include <boost/algorithm/hex.hpp>

#include "Eth.h"
#include "AccountHolder.h"
#include "JsonHelper.h"

using namespace std;
using namespace jsonrpc;
using namespace dev;
using namespace eth;
using namespace shh;
using namespace dev::rpc;

#if ETH_DEBUG
const unsigned dev::SensibleHttpThreads = 8;
#else
const unsigned dev::SensibleHttpThreads = 8;
#endif
const unsigned dev::SensibleHttpPort = 6789;

Eth::Eth(eth::Interface& _eth, eth::AccountHolder& _ethAccounts):
	m_eth(_eth),
	m_ethAccounts(_ethAccounts)
{
}

string Eth::eth_protocolVersion()
{
	return toJS(eth::c_protocolVersion);
}

string Eth::eth_coinbase()
{
	return toJS(client()->author());
}

string Eth::eth_hashrate()
{
	return "1";
	/*
	try
	{
		return toJS(asEthashClient(client())->hashrate());
	}
	catch (InvalidSealEngine&)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}*/
}

bool Eth::eth_mining()
{
	return false;
	/*
	try
	{
		return asEthashClient(client())->isMining();
	}
	catch (InvalidSealEngine&)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}*/
}

string Eth::eth_gasPrice()
{
	return toJS(client()->gasBidPrice());
}

Json::Value Eth::eth_accounts()
{
	return toJson(m_ethAccounts.allAccounts());
}

string Eth::eth_blockNumber()
{
	return toJS(client()->number());
}
string Eth::eth_pbftView()
{
	try {
		if (dynamic_cast<PBFT*>(client()->sealEngine())) {
			return toJS(dynamic_cast<PBFT*>(client()->sealEngine())->view());
		}
	} catch (...) {
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
	return "";
}

Json::Value Eth::eth_getProofMerkle(string const& _blockHash, string const& _transactionIndex)
{
    h256 h = jsToFixed<32>(_blockHash);
    if (!client()->isKnown(h)) {
        return Json::Value(Json::nullValue);
    }

    unsigned transactionIndex = jsToInt(_transactionIndex);
    auto transactions = client()->transactions(h);
    if (transactions.size() <= transactionIndex) {
        return Json::Value(Json::nullValue);
    }

    MemoryDB m;
    GenericTrieDB<MemoryDB> trieDb(&m);
    trieDb.init();

    /*
     **try to build MPT tree from transcations from block
     **key is the transaction's index in blockk,value is transaction
     */

    for (unsigned i = 0; i < transactions.size(); ++i)
    {
        try
        {
            Transaction transaction(transactions[i]);
            RLPStream k;
            k << i;

            RLPStream txrlp;
            transaction.streamRLP(txrlp);
            trieDb.insert(k.out(), txrlp.out());
        } catch(std::exception &e)
        {
            BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INTERNAL_ERROR, e.what()));
        } catch(...)
        {
            BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INTERNAL_ERROR, "construct trie error"));
        }
    }

    try
    {
        RLPStream key;
        key << transactionIndex;

        std::string targetTransactionValue = trieDb.at(key.out());
        std::vector<std::string> proofs = trieDb.proofs(key.out());
        BlockHeader blockHeader = client()->blockInfo(h);
        h256 transactionRoot = blockHeader.transactionsRoot();
        h512s nodeList = blockHeader.nodeList();
        
        Json::Value result(Json::objectValue);

        Json::Value jsonProofs(Json::arrayValue);
        for(std::string proof : proofs)
        {
            jsonProofs.append(toHex(proof));
        }

        Json::Value jsonNodePubs(Json::arrayValue);
        for(h512 pub : nodeList)
        {
            jsonNodePubs.append(pub.hex());
        }

        auto blockBytes = dynamic_cast<ClientBase*>(client())->blockRLP(h256(_blockHash));
        
        RLP rlpBlock(blockBytes);
        
        if (rlpBlock.itemCount() >= 5)
        {
            Json::Value jsonSigns(Json::arrayValue);
            std::vector<std::pair<u256, Signature>> signPairVect = rlpBlock[4].toVector<std::pair<u256, Signature>>();
            for (auto signPair : signPairVect)
            {
                Json::Value idxAndSign(Json::objectValue);
                idxAndSign["idx"] = signPair.first.str();
                idxAndSign["sign"] = signPair.second.hex();
                jsonSigns.append(idxAndSign);
            }
            
            result["signs"] = jsonSigns;
            
            h256 hash = rlpBlock[3].toHash<h256>();
            result["hash"] = hash.hex();
        }
        
        result["root"] = transactionRoot.hex();
        result["proofs"] = jsonProofs;
        result["pubs"] = jsonNodePubs;
        result["key"] = _transactionIndex;
        result["value"] = sha3(targetTransactionValue).hex();

        return result;
    } catch(std::exception &e)
    {
        BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INTERNAL_ERROR, e.what()));
    } catch(...)
    {
        BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INTERNAL_ERROR, "get proof error"));
    }

    return Json::Value(Json::nullValue);
}

string Eth::eth_getBalance(string const& _address, string const& _blockNumber)
{
	try
	{
		return toJS(client()->balanceAt(jsToAddress(_address), jsToBlockNumber(_blockNumber)));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

string Eth::eth_getStorageAt(string const& _address, string const& _position, string const& _blockNumber)
{
	try
	{
		return toJS(toCompactBigEndian(client()->stateAt(jsToAddress(_address), jsToU256(_position), jsToBlockNumber(_blockNumber)), 32));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

string Eth::eth_getStorageRoot(string const& _address, string const& _blockNumber)
{
	try
	{
		return toString(client()->stateRootAt(jsToAddress(_address), jsToBlockNumber(_blockNumber)));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

/*
string Eth::eth_pendingTransactions()
{
	//Return list of transaction that being sent by local accounts
	Transactions ours;
	for (Transaction const& pending : client()->pending())
	{
		for (Address const& account : m_ethAccounts.allAccounts())
		{
			if (pending.sender() == account)
			{
				ours.push_back(pending);
				break;
			}
		}
	}

	return toJS(ours);
}
*/

Json::Value Eth::eth_pendingTransactions()
{
	try
	{
		BlockHeader bi = client()->pendingInfo();
		Json::Value res;

		//Current
		res["current"] = Json::Value(Json::arrayValue);;
		unsigned i = 0;
		for (Transaction const& tx : client()->transactionQueue().allTransactions())
		{
			res["current"].append(toJson(tx, std::make_pair(bi.hash(), i++), (BlockNumber)bi.number()));
		}

		bi = client()->pendingInfo();//再查一遍
		//Pending
		res["pending"] = Json::Value(Json::arrayValue);
		i = 0;
		for (Transaction const& tx : client()->pending())
		{
			res["pending"].append(toJson(tx, std::make_pair(bi.hash(), i++), (BlockNumber)bi.number()));
		}
		return res;
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}		
}

string Eth::eth_getTransactionCount(string const& _address, string const& _blockNumber)
{
	try
	{
		return toJS(client()->countAt(jsToAddress(_address), jsToBlockNumber(_blockNumber)));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getBlockTransactionCountByHash(string const& _blockHash)
{
	try
	{
		h256 blockHash = jsToFixed<32>(_blockHash);
		if (!client()->isKnown(blockHash))
			return Json::Value(Json::nullValue);

		return toJS(client()->transactionCount(blockHash));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getBlockTransactionCountByNumber(string const& _blockNumber)
{
	try
	{
		BlockNumber blockNumber = jsToBlockNumber(_blockNumber);
		if (!client()->isKnown(blockNumber))
			return Json::Value(Json::nullValue);

		return toJS(client()->transactionCount(jsToBlockNumber(_blockNumber)));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getUncleCountByBlockHash(string const& _blockHash)
{
	try
	{
		h256 blockHash = jsToFixed<32>(_blockHash);
		if (!client()->isKnown(blockHash))
			return Json::Value(Json::nullValue);

		return toJS(client()->uncleCount(blockHash));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getUncleCountByBlockNumber(string const& _blockNumber)
{
	try
	{
		BlockNumber blockNumber = jsToBlockNumber(_blockNumber);
		if (!client()->isKnown(blockNumber))
			return Json::Value(Json::nullValue);

		return toJS(client()->uncleCount(blockNumber));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

string Eth::eth_getCode(string const& _address, string const& _blockNumber)
{
	try
	{
		return toJS(client()->codeAt(jsToAddress(_address), jsToBlockNumber(_blockNumber)));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

void Eth::setTransactionDefaults(TransactionSkeleton& _t)
{
	if (!_t.from)
		_t.from = m_ethAccounts.defaultTransactAccount();
}

string Eth::eth_sendTransaction(Json::Value const& _json)
{
	try
	{
		//std::cout << "eth_sendTransaction " << _json << "\n";

		TransactionSkeleton t = toTransactionSkeleton(_json);
		setTransactionDefaults(t);

		CnsParams params;
		if (isOldCNSCall(t, params, _json))
		{//CNS v1
			eth_sendTransactionOldCNSSetParams(t);
		}
		else if (isNewCNSCall(t))
		{//CNS v2
			eth_sendTransactionNewCNSSetParams(t);
		}

		if ( t.blockLimit == Invalid256 ) //by default
			t.blockLimit = client()->number() + 100;


		TransactionNotification n = m_ethAccounts.authenticate(t);
		LOG(TRACE) << "Eth::eth_sendTransaction " << n.hash << "," << (unsigned)n.r;

		switch (n.r)
		{
		case TransactionRepercussion::Success:
			return toJS(n.hash);
		case TransactionRepercussion::ProxySuccess:
			return toJS(n.hash);// TODO: give back something more useful than an empty hash.
		case TransactionRepercussion::NonceError:
		{
			BOOST_THROW_EXCEPTION(JsonRpcException("Randomid   Check Fail."));
		}
		case TransactionRepercussion::BlockLimitError:
		{
			BOOST_THROW_EXCEPTION(JsonRpcException("BlockLimit  Check Fail."));
		}
		case TransactionRepercussion::TxPermissionError:
		{
			BOOST_THROW_EXCEPTION(JsonRpcException("TxPermission  ."));
		}
		case TransactionRepercussion::DeployPermissionError:
		{
			BOOST_THROW_EXCEPTION(JsonRpcException("DeployPermissionError  ."));
		}
		case TransactionRepercussion::UnknownAccount:
			BOOST_THROW_EXCEPTION(JsonRpcException("Account unknown."));
		case TransactionRepercussion::Locked:
			BOOST_THROW_EXCEPTION(JsonRpcException("Account is locked."));
		case TransactionRepercussion::Refused:
			BOOST_THROW_EXCEPTION(JsonRpcException("Transaction rejected by user."));
		case TransactionRepercussion::Unknown:
			BOOST_THROW_EXCEPTION(JsonRpcException("Unknown reason."));

		default:
			BOOST_THROW_EXCEPTION(JsonRpcException("UnHandler reason."));
		}
	}
	catch (JsonRpcException&)
	{
		LOG(ERROR) << boost::current_exception_diagnostic_information();
		throw;
	}
	catch (...)
	{
		LOG(ERROR) << boost::current_exception_diagnostic_information();
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
	BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	return string();
}

string Eth::eth_signTransaction(Json::Value const& _json)
{
	try
	{
		TransactionSkeleton t = toTransactionSkeleton(_json);
		setTransactionDefaults(t);
		TransactionNotification n = m_ethAccounts.authenticate(t);
		switch (n.r)
		{
		case TransactionRepercussion::Success:
			return toJS(n.hash);
		case TransactionRepercussion::ProxySuccess:
			return toJS(n.hash);// TODO: give back something more useful than an empty hash.
		default:
			// TODO: provide more useful information in the exception.
			BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
		}
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_inspectTransaction(std::string const& _rlp)
{
	try
	{
		return toJson(Transaction(jsToBytes(_rlp, OnFailed::Throw), CheckTransaction::Everything));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

string Eth::eth_sendRawTransaction(std::string const& _rlp)
{
	try
	{
		auto tx_data = jsToBytes(_rlp, OnFailed::Throw);
		std::pair<ImportResult, h256> ret = client()->injectTransaction(tx_data);//when do import, it will use CheckTransaction::Everything for check
		ImportResult ir = ret.first;
		if ( ImportResult::Success == ir)
		{
			return toJS(ret.second);
			//Transaction tx(tx_data, CheckTransaction::None);
			//LOG(INFO) << "eth_sendRawTransaction Hash=" << (tx.sha3()) << ",Randid=" << tx.randomid() << ",RPC=" << utcTime();

			//return toJS(tx.sha3());
		}
		else if ( ImportResult::NonceCheckFail == ir )
		{
			BOOST_THROW_EXCEPTION(JsonRpcException("Nonce Check Fail."));
		}
		else if ( ImportResult::BlockLimitCheckFail == ir )
		{
			BOOST_THROW_EXCEPTION(JsonRpcException("BlockLimit Check Fail."));
		}
		else if ( ImportResult::NoTxPermission == ir )
		{
			BOOST_THROW_EXCEPTION(JsonRpcException("NoTxPermission ."));
		}
		else if ( ImportResult::NoDeployPermission == ir )
		{
			BOOST_THROW_EXCEPTION(JsonRpcException("NoDeployPermission ."));
		}
		else if ( ImportResult::Malformed == ir )
		{
			BOOST_THROW_EXCEPTION(JsonRpcException("Malformed!!"));
		}
		else
		{
			BOOST_THROW_EXCEPTION(JsonRpcException(" Something Fail!"));
		}

	}
	catch (JsonRpcException const& _e)
	{
		//return _e.GetMessage();
		throw;
	}
	catch (...)
	{

		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

//add jsonCall
Json::Value Eth::eth_jsonCall(Json::Value const& _json, std::string const& _blockNumber)
{
	/*
	//request
	{
		"jsonrpc": "2.0",
		"method" : "eth_json_call",
		"params" : [
			{
				"contract": "",
				"func":"",
				"version":"",
				"params" : []
			}
					],
			"id": 1
	}

	//response
	{
		"id": 7,
		"jsonrpc" : "2.0",
		"result"  : {
		"ret_code":,
		"ret_msg" : ,
		"result"  : []
		}
	}

	//jsonCall process:
	1. parse name =>  name format:contract name-function-version attention:version can be ignored 。
	2. get abi info and contract address by contract name and version
	3. then call the evm with params
	*/

	LOG(DEBUG) << "eth_jsonCall begin, blocknumber= " << _blockNumber << "json=" << _json.toStyledString();

	try
	{
		CnsParams params;
		//parse params
		fromJsonGetParams(_json, params);

		libabi::SolidityAbi abiinfo;
		//get abi info
		libabi::ContractAbiMgr::getInstance()->getContractAbi(params.strContractName, params.strVersion, abiinfo);

		//encode abi
		const auto &f = abiinfo.getFunction(params.strFunc);
		//do call for non-constant function in contract
		if (!f.bConstant())
		{
			ABI_EXCEPTION_THROW("call on not constant function ,contract|func|version=" + params.strContractName + "|" + params.strFunc + "|" + params.strVersion, libabi::EnumAbiExceptionErrCode::EnumAbiExceptionErrCodeInvalidAbiTransactionOnConstantFunc);
		}

		Json::Value jResult(Json::objectValue);
		Json::Value jReturn;
		{
			//abi info
			auto strData = libabi::SolidityCoder::getInstance()->encode(f, params.jParams);

        
			TransactionSkeleton t;
			setTransactionDefaults(t);
			//get contract address
			t.to = jsToAddress(abiinfo.getAddr());
			t.data = jsToBytes(strData);
			ExecutionResult er = client()->call(t.from, t.value, t.to, t.data, t.gas, t.gasPrice, jsToBlockNumber(_blockNumber), FudgeFactor::Lenient);
			//abi decode
			jReturn = libabi::SolidityCoder::getInstance()->decode(f, toJS(er.output));
		}

		jResult["ret_code"] = 0;
		jResult["ret_msg"]  = "success!";
		jResult["result"]   = jReturn;

		LOG(DEBUG) << "jsonCall end success"
		           << " ,params=" << params.jParams.toStyledString()
		           << " ,result=" << jResult.toStyledString();

		return jResult;
	}
	catch (const libabi::AbiException &e)
	{
		Json::Value jResult(Json::objectValue);
		jResult["ret_code"] = static_cast<unsigned int>(e.error_code());
		jResult["ret_msg"]  = e.what();
		jResult["result"]   = "";

		LOG(WARNING) << "jsonCall end abi exception"
		             << " ,json=" << _json.toStyledString()
		             << " ,msg=" << e.what()
		             ;

		return jResult;
	}
	catch ( NoCallPermission const &in)
	{
		LOG(ERROR) << boost::current_exception_diagnostic_information() << "\n";
		BOOST_THROW_EXCEPTION(JsonRpcException("NoCallPermission."));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}


string Eth::eth_call(Json::Value const& _json, string const& _blockNumber)
{
	try
	{
		LOG(DEBUG) << "eth_call # _json = " << _json.toStyledString();

		TransactionSkeleton t = toTransactionSkeleton(_json);
		setTransactionDefaults(t);

		std::string result;
		CnsParams params;
		if (isOldCNSCall(t, params, _json))
		{//CNS v1
			result = eth_callOldCNS(t, params, _blockNumber);
		}
		else if (isNewCNSCall(t))
		{//CNS v2
			result = eth_callNewCNS(t, _blockNumber);
		}
		else
		{//default
			result = eth_callDefault(t, _blockNumber);
		}

		return result;
	}
	catch ( NoCallPermission const &in)
	{
		LOG(ERROR) << boost::current_exception_diagnostic_information() << "\n";
		BOOST_THROW_EXCEPTION(JsonRpcException("NoCallPermission."));
	}
	catch (...)
	{
		LOG(ERROR) << boost::current_exception_diagnostic_information() << "\n";
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

string Eth::eth_estimateGas(Json::Value const& _json)
{
	try
	{
		TransactionSkeleton t = toTransactionSkeleton(_json);
		setTransactionDefaults(t);
		return toJS(client()->estimateGas(t.from, t.value, t.to, t.data, t.gas, t.gasPrice, PendingBlock).first);
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

bool Eth::eth_flush()
{
	client()->flushTransactions();
	return true;
}

Json::Value Eth::eth_getBlockByHash(string const& _blockHash, bool _includeTransactions)
{
	try
	{
		h256 h = jsToFixed<32>(_blockHash);
		if (!client()->isKnown(h))
			return Json::Value(Json::nullValue);

		if (_includeTransactions)
			return toJson(client()->blockInfo(h), client()->blockDetails(h), client()->uncleHashes(h), client()->transactions(h), client()->sealEngine());
		else
			return toJson(client()->blockInfo(h), client()->blockDetails(h), client()->uncleHashes(h), client()->transactionHashes(h), client()->sealEngine());
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getBlockByNumber(string const& _blockNumber, bool _includeTransactions)
{
	try
	{
		BlockNumber h = jsToBlockNumber(_blockNumber);
		if (!client()->isKnown(h))
			return Json::Value(Json::nullValue);

		if (_includeTransactions)
			return toJson(client()->blockInfo(h), client()->blockDetails(h), client()->uncleHashes(h), client()->transactions(h), client()->sealEngine());
		else
			return toJson(client()->blockInfo(h), client()->blockDetails(h), client()->uncleHashes(h), client()->transactionHashes(h), client()->sealEngine());
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getTransactionByHash(string const& _transactionHash)
{
	try
	{
		h256 h = jsToFixed<32>(_transactionHash);
		if (!client()->isKnownTransaction(h))
			return Json::Value(Json::nullValue);

		return toJson(client()->localisedTransaction(h));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getTransactionByBlockHashAndIndex(string const& _blockHash, string const& _transactionIndex)
{
	try
	{
		h256 bh = jsToFixed<32>(_blockHash);
		unsigned ti = jsToInt(_transactionIndex);
		if (!client()->isKnownTransaction(bh, ti))
			return Json::Value(Json::nullValue);

		return toJson(client()->localisedTransaction(bh, ti));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getTransactionByBlockNumberAndIndex(string const& _blockNumber, string const& _transactionIndex)
{
	try
	{
		BlockNumber bn = jsToBlockNumber(_blockNumber);
		h256 bh = client()->hashFromNumber(bn);
		unsigned ti = jsToInt(_transactionIndex);
		if (!client()->isKnownTransaction(bh, ti))
			return Json::Value(Json::nullValue);

		return toJson(client()->localisedTransaction(bh, ti));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getTransactionReceipt(string const& _transactionHash)
{
	try
	{
		h256 h = jsToFixed<32>(_transactionHash);
		if (!client()->isKnownTransaction(h))
			return Json::Value(Json::nullValue);

		return toJson(client()->localisedTransactionReceipt(h));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getUncleByBlockHashAndIndex(string const& _blockHash, string const& _uncleIndex)
{
	try
	{
		return toJson(client()->uncle(jsToFixed<32>(_blockHash), jsToInt(_uncleIndex)), client()->sealEngine());
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getUncleByBlockNumberAndIndex(string const& _blockNumber, string const& _uncleIndex)
{
	try
	{
		return toJson(client()->uncle(jsToBlockNumber(_blockNumber), jsToInt(_uncleIndex)), client()->sealEngine());
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

string Eth::eth_newFilter(Json::Value const& _json)
{
	try
	{
		return toJS(client()->installWatch(toLogFilter(_json, *client())));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

string Eth::eth_newFilterEx(Json::Value const& _json)
{
	try
	{
		return toJS(client()->installWatch(toLogFilter(_json)));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

string Eth::eth_newBlockFilter()
{
	h256 filter = dev::eth::ChainChangedFilter;
	return toJS(client()->installWatch(filter));
}

string Eth::eth_newPendingTransactionFilter()
{
	h256 filter = dev::eth::PendingChangedFilter;
	return toJS(client()->installWatch(filter));
}

bool Eth::eth_uninstallFilter(string const& _filterId)
{
	try
	{
		return client()->uninstallWatch(jsToInt(_filterId));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getFilterChanges(string const& _filterId)
{
	try
	{
		int id = jsToInt(_filterId);
		auto entries = client()->checkWatch(id);
//		if (entries.size())
//			LOG(INFO) << "FIRING WATCH" << id << entries.size();
		return toJson(entries);
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getFilterChangesEx(string const& _filterId)
{
	try
	{
		int id = jsToInt(_filterId);
		auto entries = client()->checkWatch(id);
//		if (entries.size())
//			LOG(INFO) << "FIRING WATCH" << id << entries.size();
		return toJsonByBlock(entries);
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getFilterLogs(string const& _filterId)
{
	try
	{
		return toJson(client()->logs(jsToInt(_filterId)));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getFilterLogsEx(string const& _filterId)
{
	try
	{
		return toJsonByBlock(client()->logs(jsToInt(_filterId)));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getLogs(Json::Value const& _json)
{
	try
	{
		return toJson(client()->logs(toLogFilter(_json, *client())));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getLogsEx(Json::Value const& _json)
{
	try
	{
		return toJsonByBlock(client()->logs(toLogFilter(_json)));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_getWork()
{
	Json::Value ret(Json::arrayValue);
	return ret;
	/*
	try
	{
		Json::Value ret(Json::arrayValue);
		auto r = asEthashClient(client())->getEthashWork();
		ret.append(toJS(get<0>(r)));
		ret.append(toJS(get<1>(r)));
		ret.append(toJS(get<2>(r)));
		return ret;
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}*/
}

Json::Value Eth::eth_syncing()
{
	dev::eth::SyncStatus sync = client()->syncStatus();
	if (sync.state == SyncState::Idle || !sync.majorSyncing)
		return Json::Value(false);

	Json::Value info(Json::objectValue);
	info["startingBlock"] = sync.startBlockNumber;
	info["highestBlock"] = sync.highestBlockNumber;
	info["currentBlock"] = sync.currentBlockNumber;
	return info;
}

bool Eth::eth_submitWork(string const& , string const&, string const& )
{
	return false;
	/*
	try
	{
		return asEthashClient(client())->submitEthashWork(jsToFixed<32>(_mixHash), jsToFixed<Nonce::size>(_nonce));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}*/
}

bool Eth::eth_submitHashrate(string const& , string const& )
{
	return false;
	/*
	try
	{
		asEthashClient(client())->submitExternalHashrate(jsToInt<32>(_hashes), jsToFixed<32>(_id));
		return true;
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}*/
}

string Eth::eth_register(string const& _address)
{
	try
	{
		return toJS(m_ethAccounts.addProxyAccount(jsToAddress(_address)));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

bool Eth::eth_unregister(string const& _accountId)
{
	try
	{
		return m_ethAccounts.removeProxyAccount(jsToInt(_accountId));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

Json::Value Eth::eth_fetchQueuedTransactions(string const& _accountId)
{
	try
	{
		auto id = jsToInt(_accountId);
		Json::Value ret(Json::arrayValue);
		// TODO: throw an error on no account with given id
		for (TransactionSkeleton const& t : m_ethAccounts.queuedTransactions(id))
			ret.append(toJson(t));
		m_ethAccounts.clearQueue(id);
		return ret;
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

string Eth::eth_unverifiedBlockQueueSize()
{
	try {
		std::size_t size = client()->noConstblockQueue().unverifiedQueueSize()
						+client()->noConstblockQueue().verifyingQueueSize();
		return toJS(size);
	} catch (...) {
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
	return "";
}

string Eth::eth_verifiedBlockQueueSize()
{
	try {
		std::size_t size = client()->noConstblockQueue().verifiedQueueSize();
		return toJS(size);
	} catch (...) {
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
	return "";
}

string Eth::eth_unverifiedTransactionsQueueSize()
{
	try {
		std::size_t size = client()->transactionQueue().unverifiedSize();
		return toJS(size);
	} catch (...) {
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
	return "";
}

string Eth::eth_verifiedTransactionsQueueSize()
{
	try {
		std::size_t size = client()->transactionQueue().verifiedSize();
		return toJS(size);
	} catch (...) {
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
	return "";
}

std::string Eth::eth_getCodeCNS(std::string const& strContractName, std::string const& _blockNumber)
{
	try
	{
		LOG(TRACE) << "getCodeCNS begin , contract name = " << strContractName;

		auto rVectorString = libabi::SolidityTools::splitString(strContractName, libabi::SolidityTools::CNS_SPLIT_STRING);
		std::string contract = (!rVectorString.empty() ? rVectorString[0] : "");
		std::string version = (rVectorString.size() > 1 ? rVectorString[1] : "");

		LOG(TRACE) << "getCodeCNS ## contract = " << contract << " ,version = " << version;

		// get abi info
		libabi::SolidityAbi abiinfo;
		libabi::ContractAbiMgr::getInstance()->getContractAbi(contract, version, abiinfo);
		LOG(DEBUG) << "getCodeCNS ## contract address is => " << abiinfo.getAddr();

		return toJS(client()->codeAt(jsToAddress(abiinfo.getAddr()), jsToBlockNumber(_blockNumber)));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

std::string Eth::eth_getBalanceCNS(std::string const& strContractName, std::string const& _blockNumber)
{
	try
	{
		LOG(TRACE) << "getBalanceCNS begin , contract name = " << strContractName;

		auto rVectorString = libabi::SolidityTools::splitString(strContractName, libabi::SolidityTools::CNS_SPLIT_STRING);
		std::string contract = (!rVectorString.empty() ? rVectorString[0] : "");
		std::string version = (rVectorString.size() > 1 ? rVectorString[1] : "");

		LOG(TRACE) << "getBalanceCNS ## contract = " << contract << " ,version = " << version;

		// get abi info
		libabi::SolidityAbi abiinfo;
		libabi::ContractAbiMgr::getInstance()->getContractAbi(contract, version, abiinfo);

		LOG(DEBUG) << "getBalanceCNS ## contract address is => " << abiinfo.getAddr();

		return toJS(client()->balanceAt(jsToAddress(abiinfo.getAddr()), jsToBlockNumber(_blockNumber)));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

std::string Eth::eth_getStorageAtCNS(std::string const& strContractName, std::string const& _position, std::string const& _blockNumber)
{
	try
	{
		LOG(TRACE) << "getStorageAtCNS begin , contract name = " << strContractName;

		auto rVectorString = libabi::SolidityTools::splitString(strContractName, libabi::SolidityTools::CNS_SPLIT_STRING);
		std::string contract = (!rVectorString.empty() ? rVectorString[0] : "");
		std::string version = (rVectorString.size() > 1 ? rVectorString[1] : "");

		LOG(TRACE) << "getStorageAtCNS ## contract = " << contract << " ,version = " << version;

		// get abi info
		libabi::SolidityAbi abiinfo;
		libabi::ContractAbiMgr::getInstance()->getContractAbi(contract, version, abiinfo);
		LOG(DEBUG) << "getStorageAtCNS ## contract address is => " << abiinfo.getAddr();

		return toJS(toCompactBigEndian(client()->stateAt(jsToAddress(abiinfo.getAddr()), jsToU256(_position), jsToBlockNumber(_blockNumber)), 32));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

std::string Eth::eth_getTransactionCountCNS(std::string const& strContractName, std::string const& _blockNumber)
{
	try
	{
		LOG(TRACE) << "getTransactionCountCNS begin, contract name = " << strContractName;

		auto rVectorString = libabi::SolidityTools::splitString(strContractName, libabi::SolidityTools::CNS_SPLIT_STRING);
		std::string contract = (!rVectorString.empty() ? rVectorString[0] : "");
		std::string version = (rVectorString.size() > 1 ? rVectorString[1] : "");

		LOG(TRACE) << "getTransactionCountCNS ## contract = " << contract << " ,version = " << version;

		// get abi info
		libabi::SolidityAbi abiinfo;
		libabi::ContractAbiMgr::getInstance()->getContractAbi(contract, version, abiinfo);
		LOG(DEBUG) << "getTransactionCountCNS ## contract address = " << abiinfo.getAddr();

		return toJS(client()->countAt(jsToAddress(abiinfo.getAddr()), jsToBlockNumber(_blockNumber)));
	}
	catch (...)
	{
		BOOST_THROW_EXCEPTION(JsonRpcException(Errors::ERROR_RPC_INVALID_PARAMS));
	}
}

bool Eth::isDefaultCall(const TransactionSkeleton &t, CnsParams &params, const Json::Value &_json)
{
	return !isOldCNSCall(t,params,_json) && !isNewCNSCall(t);
}

bool Eth::isOldCNSCall(const TransactionSkeleton &t, CnsParams &params, const Json::Value &_json)
{
	//in CNS, data is json object
	if (!t.jData.empty())
	{
		fromJsonGetParams(t.jData, params); //parse params
		return true;
	}
 
	//compatible with web3j
	string strData = _json["data"].asString();
	if (strData.compare(0, 2, "0x") == 0 || strData.compare(0, 2, "0X") == 0)
	{
		strData.erase(0, 2);
	}

	string json;
	boost::algorithm::unhex(strData.begin(), strData.end(), std::back_inserter(json));

	return fromJsonGetParams(json, params);
}

bool Eth::isNewCNSCall(const TransactionSkeleton &t)
{
	return !t.strContractName.empty();
}

void Eth::eth_sendTransactionOldCNSSetParams(TransactionSkeleton &t)
{
	CnsParams params;
	fromJsonGetParams(t.jData, params);
	auto r = libabi::ContractAbiMgr::getInstance()->getAddrAndDataFromCache(params.strContractName, params.strFunc, params.strVersion, params.jParams);
	t.to   = r.first;
	t.data = r.second;
	t.creation = false;

	LOG(DEBUG) << "sendTransactionOldCNS # address = " << ("0x" + t.to.hex())
		<< " ,contract = " << params.strContractName
		<< " ,func = " << params.strFunc
		<< " ,version = " << params.strVersion
		;
}

void Eth::eth_sendTransactionNewCNSSetParams(TransactionSkeleton &t)
{
	//1. get contract name and version
	auto rVectorString = libabi::SolidityTools::splitString(t.strContractName, libabi::SolidityTools::CNS_SPLIT_STRING);
	std::string contract = (!rVectorString.empty() ? rVectorString[0] : "");
	std::string version = (rVectorString.size() > 1 ? rVectorString[1] : "");

	LOG(DEBUG) << "sendTransactionNewCNS ## contract = " << contract << " ,version = " << version;

	//2. get abi info
	libabi::SolidityAbi abiinfo;
	libabi::ContractAbiMgr::getInstance()->getContractAbi(contract, version, abiinfo);
	LOG(DEBUG) << "sendTransactionNewCNS ## contract_name = " << t.strContractName << " ,contract_address = " << abiinfo.getAddr();

	//3. get contract address
	t.to = dev::jsToAddress(abiinfo.getAddr());
	t.creation = false;
}

std::string Eth::eth_callDefault(TransactionSkeleton &t, std::string const& _blockNumber)
{
	LOG(DEBUG) << "eth_callDefault # " ;
	ExecutionResult er = client()->call(t.from, t.value, t.to, t.data, t.gas, t.gasPrice, jsToBlockNumber(_blockNumber), FudgeFactor::Lenient);
	return toJS(er.output);
}

std::string Eth::eth_callOldCNS(TransactionSkeleton &t, const CnsParams &params, std::string const& _blockNumber)
{
	LOG(DEBUG) << "eth_callOldCNS # contract|version|func|params => " 
		<< params.strContractName << "|"
		<< params.strVersion << "|"
		<< params.strFunc << "|"
		<< params.jParams.toStyledString();

	libabi::SolidityAbi abiinfo;
	//2. get abi info
	libabi::ContractAbiMgr::getInstance()->getContractAbi(params.strContractName, params.strVersion, abiinfo);

	//encode abi
	const auto &f = abiinfo.getFunction(params.strFunc);
	//do call invoke in non-constant function
	if (!f.bConstant())
	{
		ABI_EXCEPTION_THROW("call on not constant function ,contract|func|version=" + params.strContractName + "|" + params.strFunc + "|" + params.strVersion, libabi::EnumAbiExceptionErrCode::EnumAbiExceptionErrCodeInvalidAbiTransactionOnConstantFunc);
	}

	//3. abi serialize
	std::string strData = libabi::SolidityCoder::getInstance()->encode(abiinfo.getFunction(params.strFunc), params.jParams);
	//4. call contract
	t.to = dev::jsToAddress(abiinfo.getAddr());
	t.data = dev::jsToBytes(strData);

	LOG(DEBUG) << "eth_callOldCNS # address => " << abiinfo.getAddr();
	//5. do call
	ExecutionResult er = client()->call(t.from, t.value, t.to, t.data, t.gas, t.gasPrice, jsToBlockNumber(_blockNumber), FudgeFactor::Lenient);
	//6. decode result
	auto jReturn = libabi::SolidityCoder::getInstance()->decode(f, toJS(er.output));

	Json::FastWriter writer;
	std::string out = writer.write(jReturn);

	LOG(DEBUG) << "eth_callOldCNS # result = " << out;

	return out;
}

std::string Eth::eth_callNewCNS(TransactionSkeleton &t, std::string const& _blockNumber)
{
	//1. get contract name and version
	auto rVectorString = libabi::SolidityTools::splitString(t.strContractName, libabi::SolidityTools::CNS_SPLIT_STRING);
	std::string contract = (!rVectorString.empty() ? rVectorString[0] : "");
	std::string version = (rVectorString.size() > 1 ? rVectorString[1] : "");

	LOG(TRACE) << "eth_callNewCNS ## contract = " << contract << " ,version = " << version;

	//2. get abi info
	libabi::SolidityAbi abiinfo;
	libabi::ContractAbiMgr::getInstance()->getContractAbi(contract, version, abiinfo);
	LOG(DEBUG) << "eth_callNewCNS ## contract_address = " << abiinfo.getAddr();

	//3. get contract address
	t.to = dev::jsToAddress(abiinfo.getAddr());

	//4. call invoke
	ExecutionResult er = client()->call(t.from, t.value, t.to, t.data, t.gas, t.gasPrice, jsToBlockNumber(_blockNumber), FudgeFactor::Lenient);

	return toJS(er.output);
}

Json::Value Eth::eth_getCmByRange(Json::Value const &range)
{
	/*
curl --data '{"jsonrpc":"2.0","method":"eth_getCmByRange","id":1,"params":[{"from":0,"to":-1}]}' localhost:8545
*/
	Json::Value ret;
	ret["from"] = 0;
	ret["to"] = 0;
	Json::Value cms(Json::arrayValue);
	try
	{
		// [from， to)
		int64_t from = range["from"].asInt64();
		int64_t to = range["to"].asInt64();

		CMPool_Singleton &pool = CMPool_Singleton::Instance();
		poolBlockNumber_t cbn = client()->number();

		if (to <= 0)
			to = (int64_t)pool.size(cbn);
		else
			to = min(to, (int64_t)pool.size(cbn));
		//LOG(TRACE) << "pool size: " << pool.size(cbn) << std::endl;

		ret["from"] = from;
		ret["to"] = to;

		//[from,to) error return [null]
		if (from >= to || from < 0)
		{
			ret["cms"] = cms;
			return ret;
		}

		for (; from < to; from++)
		{
			cms.append(pool.get(cbn, from));
		}
	}
	catch (std::exception &e)
	{
		LOG(ERROR) << e.what() << endl;
	}
	ret["cms"] = cms;
	return ret;
}

Json::Value Eth::eth_getGovDataByRange(Json::Value const &range)
{
	/*
curl --data '{
"jsonrpc":"2.0","method":"eth_getGovDataByRange","id":1,"params":[{"from":0,"to":-1}]
}' localhost:8545
*/
	Json::Value ret;
	ret["from"] = 0;
	ret["to"] = 0;
	Json::Value g_datas(Json::arrayValue);
	try
	{
		// [from， to)
		int64_t from = range["from"].asInt();
		int64_t to = range["to"].asInt();

		GovDataPool_Singleton &pool = GovDataPool_Singleton::Instance();
		poolBlockNumber_t cbn = client()->number();

		if (to <= 0)
			to = (int64_t)pool.size(cbn);
		else
			to = min(to, (int64_t)pool.size(cbn));

		ret["from"] = from;
		ret["to"] = to;

		//[from,to) error return [null]
		if (from >= to || from < 0)
		{
			ret["G_datas"] = g_datas;
			return ret;
		}

		for (; from < to; from++)
		{
			g_datas.append(pool.get(cbn, from));
		}
	}
	catch (std::exception &e)
	{
		LOG(ERROR) << e.what() << endl;
	}
	ret["G_datas"] = g_datas;
	return ret;
}