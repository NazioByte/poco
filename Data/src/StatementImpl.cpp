//
// StatementImpl.cpp
//
// $Id: //poco/Main/Data/src/StatementImpl.cpp#20 $
//
// Library: Data
// Package: DataCore
// Module:  StatementImpl
//
// Copyright (c) 2006, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// Permission is hereby granted, free of charge, to any person or organization
// obtaining a copy of the software and accompanying documentation covered by
// this license (the "Software") to use, reproduce, display, distribute,
// execute, and transmit the Software, and to prepare derivative works of the
// Software, and to permit third-parties to whom the Software is furnished to
// do so, all subject to the following:
// 
// The copyright notices in the Software and this entire statement, including
// the above license grant, this restriction and the following disclaimer,
// must be included in all copies of the Software, in whole or in part, and
// all derivative works of the Software, unless such copies or derivative
// works are solely in the form of machine-executable object code generated by
// a source language processor.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
// SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
// FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//


#include "Poco/Data/StatementImpl.h"
#include "Poco/Data/SessionImpl.h"
#include "Poco/Data/DataException.h"
#include "Poco/Data/AbstractBinder.h"
#include "Poco/Data/Extraction.h"
#include "Poco/Data/LOB.h"
#include "Poco/Data/Date.h"
#include "Poco/Data/Time.h"
#include "Poco/SharedPtr.h"
#include "Poco/DateTime.h"
#include "Poco/Exception.h"


using Poco::icompare;


namespace Poco {
namespace Data {


using namespace Keywords;


const std::string StatementImpl::VECTOR = "vector";
const std::string StatementImpl::LIST = "list";
const std::string StatementImpl::DEQUE = "deque";
const std::string StatementImpl::UNKNOWN = "unknown";


StatementImpl::StatementImpl(SessionImpl& rSession):
	_state(ST_INITIALIZED),
	_extrLimit(upperLimit((Poco::UInt32) Limit::LIMIT_UNLIMITED, false)),
	_lowerLimit(0),
	_rSession(rSession),
	_storage(STORAGE_UNKNOWN_IMPL),
	_ostr(),
	_curDataSet(0),
	_bulkBinding(BULK_UNDEFINED),
	_bulkExtraction(BULK_UNDEFINED)
{
	_extractors.resize(1);
	_columnsExtracted.resize(1, 0);
}


StatementImpl::~StatementImpl()
{
}


Poco::UInt32 StatementImpl::execute()
{
	resetExtraction();
	Poco::UInt32 lim = 0;
	if (_lowerLimit > _extrLimit.value())
		throw LimitException("Illegal Statement state. Upper limit must not be smaller than the lower limit.");

	do
	{
		compile();
		if (_extrLimit.value() == Limit::LIMIT_UNLIMITED)
			lim += executeWithoutLimit();
		else
			lim += executeWithLimit();
	} while (canCompile());

	if (_extrLimit.value() == Limit::LIMIT_UNLIMITED)
		_state = ST_DONE;

	if (lim < _lowerLimit)
		throw LimitException("Did not receive enough data.");

	return lim;
}


Poco::UInt32 StatementImpl::executeWithLimit()
{
	poco_assert (_state != ST_DONE);
	Poco::UInt32 count = 0;
	Poco::UInt32 limit = _extrLimit.value();

	do
	{
		bind();
		while (count < limit && hasNext()) 
			count += next();
	} while (count < limit && canBind());

	if (!canBind() && (!hasNext() || limit == 0)) 
		_state = ST_DONE;
	else if (hasNext() && limit == count && _extrLimit.isHardLimit())
		throw LimitException("HardLimit reached (retrieved more data than requested).");
	else 
		_state = ST_PAUSED;

	return count ? count : affectedRowCount();
}


Poco::UInt32 StatementImpl::executeWithoutLimit()
{
	poco_assert (_state != ST_DONE);
	Poco::UInt32 count = 0;

	do
	{
		bind();
		while (hasNext()) count += next();
	} while (canBind());

	return count ? count : affectedRowCount();
}


void StatementImpl::compile()
{
	if (_state == ST_INITIALIZED || 
		_state == ST_RESET || 
		_state == ST_BOUND)
	{
		compileImpl();
		_state = ST_COMPILED;

		if (!extractions().size() && !isStoredProcedure())
		{
			Poco::UInt32 cols = columnsReturned();
			if (cols) makeExtractors(cols);
		}

		fixupExtraction();
		fixupBinding();
	}
}


void StatementImpl::bind()
{
	if (_state == ST_COMPILED)
	{
		bindImpl();
		_state = ST_BOUND;
	}
	else if (_state == ST_BOUND)
	{
		if (!hasNext())
		{
			if (canBind()) bindImpl();
			else _state = ST_DONE;
		}
	}
}


void StatementImpl::reset()
{
	resetBinding();
	resetExtraction();
	_state = ST_RESET;
}


void StatementImpl::setExtractionLimit(const Limit& extrLimit)
{
	if (!extrLimit.isLowerLimit())
		_extrLimit = extrLimit;
	else
		_lowerLimit = extrLimit.value();
}


void StatementImpl::setBulkExtraction(const Bulk& b)
{
	Poco::UInt32 limit = getExtractionLimit();
	if (Limit::LIMIT_UNLIMITED != limit && b.size() != limit)
		throw InvalidArgumentException("Can not set limit for statement.");

	setExtractionLimit(b.limit());
	_bulkExtraction = BULK_EXTRACTION;
}


void StatementImpl::fixupExtraction()
{
	Poco::Data::AbstractExtractionVec::iterator it    = extractions().begin();
	Poco::Data::AbstractExtractionVec::iterator itEnd = extractions().end();
	AbstractExtractor& ex = extractor();
	
	if (_curDataSet >= _columnsExtracted.size()) 
		_columnsExtracted.resize(_curDataSet + 1, 0);
	
	for (; it != itEnd; ++it)
	{
		(*it)->setExtractor(&ex);
		(*it)->setLimit(_extrLimit.value()),
		_columnsExtracted[_curDataSet] += (int)(*it)->numOfColumnsHandled();
	}
}


void StatementImpl::fixupBinding()
{
	// no need to call binder().reset(); here will be called before each bind anyway
	AbstractBindingVec::iterator it    = bindings().begin();
	AbstractBindingVec::iterator itEnd = bindings().end();
	AbstractBinder& bin = binder();
	std::size_t numRows = 0;
	if (it != itEnd) numRows = (*it)->numOfRowsHandled();
	for (; it != itEnd; ++it) (*it)->setBinder(&bin);
}


void StatementImpl::resetBinding()
{
	AbstractBindingVec::iterator it    = bindings().begin();
	AbstractBindingVec::iterator itEnd = bindings().end();
	for (; it != itEnd; ++it) (*it)->reset();
}


void StatementImpl::resetExtraction()
{
	Poco::Data::AbstractExtractionVec::iterator it = extractions().begin();
	Poco::Data::AbstractExtractionVec::iterator itEnd = extractions().end();
	for (; it != itEnd; ++it) (*it)->reset();

	poco_assert (_curDataSet < _columnsExtracted.size());
	_columnsExtracted[_curDataSet] = 0;
}


void StatementImpl::setStorage(const std::string& storage)
{
	if (0 == icompare(DEQUE, storage))
		_storage = STORAGE_DEQUE_IMPL;
	else if (0 == icompare(VECTOR, storage))
		_storage = STORAGE_VECTOR_IMPL; 
	else if (0 == icompare(LIST, storage))
		_storage = STORAGE_LIST_IMPL;
	else if (0 == icompare(UNKNOWN, storage))
		_storage = STORAGE_UNKNOWN_IMPL;
	else
		throw NotFoundException();
}


void StatementImpl::makeExtractors(Poco::UInt32 count)
{
	for (int i = 0; i < count; ++i)
	{
		const MetaColumn& mc = metaColumn(i);
		switch (mc.type())
		{
			case MetaColumn::FDT_BOOL:
				addInternalExtract<bool>(mc); break;
			case MetaColumn::FDT_INT8:  
				addInternalExtract<Int8>(mc); break;
			case MetaColumn::FDT_UINT8:  
				addInternalExtract<UInt8>(mc); break;
			case MetaColumn::FDT_INT16:  
				addInternalExtract<Int16>(mc); break;
			case MetaColumn::FDT_UINT16: 
				addInternalExtract<UInt16>(mc); break;
			case MetaColumn::FDT_INT32:  
				addInternalExtract<Int32>(mc); break;
			case MetaColumn::FDT_UINT32: 
				addInternalExtract<UInt32>(mc); break;
			case MetaColumn::FDT_INT64:  
				addInternalExtract<Int64>(mc); break;
			case MetaColumn::FDT_UINT64: 
				addInternalExtract<UInt64>(mc); break;
			case MetaColumn::FDT_FLOAT:  
				addInternalExtract<float>(mc); break;
			case MetaColumn::FDT_DOUBLE: 
				addInternalExtract<double>(mc); break;
			case MetaColumn::FDT_STRING: 
				addInternalExtract<std::string>(mc); break;
			case MetaColumn::FDT_BLOB:   
				addInternalExtract<BLOB>(mc); break;
			case MetaColumn::FDT_DATE:
				addInternalExtract<Date>(mc); break;
			case MetaColumn::FDT_TIME:
				addInternalExtract<Time>(mc); break;
			case MetaColumn::FDT_TIMESTAMP:
				addInternalExtract<DateTime>(mc); break;
			default:
				throw Poco::InvalidArgumentException("Data type not supported.");
		}
	}
}


const MetaColumn& StatementImpl::metaColumn(const std::string& name) const
{
	Poco::UInt32 cols = columnsReturned();
	for (Poco::UInt32 i = 0; i < cols; ++i)
	{
		const MetaColumn& column = metaColumn(i);
		if (0 == icompare(column.name(), name)) return column;
	}

	throw NotFoundException(format("Invalid column name: %s", name));
}


Poco::UInt32 StatementImpl::activateNextDataSet()
{
	if (_curDataSet + 1 < dataSetCount())
		return ++_curDataSet;
	else
		throw NoDataException("End of data sets reached.");
}


Poco::UInt32 StatementImpl::activatePreviousDataSet()
{
	if (_curDataSet > 0)
		return --_curDataSet;
	else
		throw NoDataException("Beginning of data sets reached.");
}


void StatementImpl::addExtract(AbstractExtraction* pExtraction)
{
	poco_check_ptr (pExtraction);
	Poco::UInt32 pos = pExtraction->position();
	if (pos >= _extractors.size()) 
		_extractors.resize(pos + 1);

	pExtraction->setEmptyStringIsNull(
		_rSession.getFeature("emptyStringIsNull"));

	pExtraction->setForceEmptyString(
		_rSession.getFeature("forceEmptyString"));

	_extractors[pos].push_back(pExtraction);
}


void StatementImpl::removeBind(const std::string& name)
{
	bool found = false;

	AbstractBindingVec::iterator it = _bindings.begin();
	for (; it != _bindings.end();)
	{
		if ((*it)->name() == name) 
		{
			it = _bindings.erase(it);
			found = true;
		}
		else ++it;
	}

	if (!found)
		throw NotFoundException(name);
}


Poco::UInt32 StatementImpl::columnsExtracted(int dataSet) const
{
	if (USE_CURRENT_DATA_SET == dataSet) dataSet = _curDataSet;
	if (_columnsExtracted.size() > 0)
	{
		poco_assert (dataSet >= 0 && dataSet < _columnsExtracted.size());
		return _columnsExtracted[dataSet];
	}

	return 0;
}


Poco::UInt32 StatementImpl::rowsExtracted(int dataSet) const
{
	if (USE_CURRENT_DATA_SET == dataSet) dataSet = _curDataSet;
	if (extractions().size() > 0)
	{
		poco_assert (dataSet >= 0 && dataSet < _extractors.size());
		if (_extractors[dataSet].size() > 0)
			return _extractors[dataSet][0]->numOfRowsHandled();
	}
	
	return 0;
}


void StatementImpl::formatSQL(std::vector<Any>& arguments)
{
	std::string sql;
	Poco::format(sql, _ostr.str(), arguments);
	_ostr.str("");	_ostr << sql;
}


} } // namespace Poco::Data
