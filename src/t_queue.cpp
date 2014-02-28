#include "t_queue.h"
#include "db_impl.h"
#include "leveldb/write_batch.h"

namespace ssdb{

//static uint64_t QSIZE_SEQ  = 1;
static uint64_t QFRONT_SEQ = 2;
static uint64_t QBACK_SEQ  = 3;
static uint64_t QITEM_MIN_SEQ = 10000;
static uint64_t QITEM_MAX_SEQ = 9223372036854775807ULL;
static uint64_t QITEM_SEQ_INIT = QITEM_MAX_SEQ/2;

static int qget(leveldb::DB* db, const Bytes &name, uint64_t seq, std::string *val){
	std::string key = encode_qitem_key(name, seq);
	leveldb::Status s;

	s = db->Get(leveldb::ReadOptions(), key, val);
	if(s.IsNotFound()){
		return 0;
	}else if(!s.ok()){
		log_error("Get() error!");
		return -1;
	}else{
		return 1;
	}
}

static int qget_uint64(leveldb::DB* db, const Bytes &name, uint64_t seq, uint64_t *ret){
	std::string val;
	*ret = 0;
	int s = qget(db, name, seq, &val);
	if(s == 1){
		if(val.size() != sizeof(uint64_t)){
			return -1;
		}
		*ret = *(uint64_t *)val.data();
	}
	return s;
}

static int qdel_one(DbImpl *ssdb, const Bytes &name, uint64_t seq){
	std::string key = encode_qitem_key(name, seq);
	leveldb::Status s;

	ssdb->writer->Delete(key);
	return 0;
}

static int qset_one(DbImpl *ssdb, const Bytes &name, uint64_t seq, const Bytes &item){
	std::string key = encode_qitem_key(name, seq);
	leveldb::Status s;

	ssdb->writer->Put(key, item);
	return 0;
}

static int64_t incr_qsize(DbImpl *ssdb, const Bytes &name, int64_t incr){
	int64_t size = ssdb->qsize(name);
	if(size == -1){
		return -1;
	}
	size += incr;
	if(size <= 0){
		ssdb->writer->Delete(encode_qsize_key(name));
		qdel_one(ssdb, name, QFRONT_SEQ);
		qdel_one(ssdb, name, QBACK_SEQ);
	}else{
		ssdb->writer->Put(encode_qsize_key(name), Bytes((char *)&size, sizeof(size)));
	}
	return size;
}

/****************/

int64_t DbImpl::qsize(const Bytes &name){
	std::string key = encode_qsize_key(name);
	std::string val;

	leveldb::Status s;
	s = db->Get(leveldb::ReadOptions(), key, &val);
	if(s.IsNotFound()){
		return 0;
	}else if(!s.ok()){
		log_error("Get() error!");
		return -1;
	}else{
		if(val.size() != sizeof(uint64_t)){
			return -1;
		}
		return *(int64_t *)val.data();
	}
}

// @return 0: empty queue, 1: item peeked, -1: error
int DbImpl::qfront(const Bytes &name, std::string *item){
	int ret = 0;
	uint64_t seq;
	ret = qget_uint64(this->db, name, QFRONT_SEQ, &seq);
	if(ret == -1){
		return -1;
	}
	if(ret == 0){
		return 0;
	}
	ret = qget(this->db, name, seq, item);
	return ret;
}

// @return 0: empty queue, 1: item peeked, -1: error
int DbImpl::qback(const Bytes &name, std::string *item){
	int ret = 0;
	uint64_t seq;
	ret = qget_uint64(this->db, name, QBACK_SEQ, &seq);
	if(ret == -1){
		return -1;
	}
	if(ret == 0){
		return 0;
	}
	ret = qget(this->db, name, seq, item);
	return ret;
}

int DbImpl::_qpush(const Bytes &name, const Bytes &item, uint64_t front_or_back_seq){
	Transaction trans(writer);

	int ret;
	// generate seq
	uint64_t seq;
	ret = qget_uint64(this->db, name, front_or_back_seq, &seq);
	if(ret == -1){
		return -1;
	}
	// update front and/or back
	if(ret == 0){
		seq = QITEM_SEQ_INIT;
		ret = qset_one(this, name, QFRONT_SEQ, Bytes(&seq, sizeof(seq)));
		if(ret == -1){
			return -1;
		}
		ret = qset_one(this, name, QBACK_SEQ, Bytes(&seq, sizeof(seq)));
	}else{
		seq += (front_or_back_seq == QFRONT_SEQ)? -1 : +1;
		ret = qset_one(this, name, front_or_back_seq, Bytes(&seq, sizeof(seq)));
	}
	if(ret == -1){
		return -1;
	}
	if(seq <= QITEM_MIN_SEQ || seq >= QITEM_MAX_SEQ){
		log_info("queue is full, seq: %" PRIu64 " out of range", seq);
		return -1;
	}
	
	// prepend/append item
	ret = qset_one(this, name, seq, item);
	if(ret == -1){
		return -1;
	}
	
	// update size
	int64_t size = incr_qsize(this, name, +1);
	if(size == -1){
		return -1;
	}

	leveldb::Status s = writer->commit();
	if(!s.ok()){
		log_error("Write error!");
		return -1;
	}
	return 1;
}

int DbImpl::qpush_front(const Bytes &name, const Bytes &item){
	return _qpush(name, item, QFRONT_SEQ);
}

int DbImpl::qpush_back(const Bytes &name, const Bytes &item){
	return _qpush(name, item, QBACK_SEQ);
}

int DbImpl::_qpop(const Bytes &name, std::string *item, uint64_t front_or_back_seq){
	Transaction trans(writer);
	
	int ret;
	uint64_t seq;
	ret = qget_uint64(this->db, name, front_or_back_seq, &seq);
	if(ret == -1){
		return -1;
	}
	if(ret == 0){
		return 0;
	}
	
	ret = qget(this->db, name, seq, item);
	if(ret == -1){
		return -1;
	}
	if(ret == 0){
		return 0;
	}

	// delete item
	ret = qdel_one(this, name, seq);
	if(ret == -1){
		return -1;
	}

	// update size
	int64_t size = incr_qsize(this, name, -1);
	if(size == -1){
		return -1;
	}
		
	// update front
	if(size > 0){
		seq += (front_or_back_seq == QFRONT_SEQ)? +1 : -1;
		//log_debug("seq: %" PRIu64 ", ret: %d", seq, ret);
		ret = qset_one(this, name, front_or_back_seq, Bytes(&seq, sizeof(seq)));
		if(ret == -1){
			return -1;
		}
	}
		
	leveldb::Status s = writer->commit();
	if(!s.ok()){
		log_error("Write error!");
		return -1;
	}
	return 1;
}

// @return 0: empty queue, 1: item popped, -1: error
int DbImpl::qpop_front(const Bytes &name, std::string *item){
	return _qpop(name, item, QFRONT_SEQ);
}

int DbImpl::qpop_back(const Bytes &name, std::string *item){
	return _qpop(name, item, QBACK_SEQ);
}

int DbImpl::qlist(const Bytes &name_s, const Bytes &name_e, uint64_t limit,
		std::vector<std::string> *list){
	std::string start;
	std::string end;
	start = encode_qsize_key(name_s);
	if(!name_e.empty()){
		end = encode_qsize_key(name_e);
	}
	Iterator *it = this->iterator(start, end, limit);
	while(it->next()){
		Bytes ks = it->key();
		//dump(ks.data(), ks.size());
		if(ks.data()[0] != DataType::QSIZE){
			break;
		}
		std::string n;
		if(decode_qsize_key(ks, &n) == -1){
			continue;
		}
		list->push_back(n);
	}
	delete it;
	return 0;
}

int DbImpl::qfix(const Bytes &name){
	Transaction trans(writer);
	std::string key_s = encode_qitem_key(name, QITEM_MIN_SEQ - 1);
	std::string key_e = encode_qitem_key(name, QITEM_MAX_SEQ);

	bool error = false;
	uint64_t seq_min = 0;
	uint64_t seq_max = 0;
	uint64_t count = 0;
	Iterator *it = this->iterator(key_s, key_e, QITEM_MAX_SEQ);
	while(it->next()){
		//dump(it->key().data(), it->key().size());
		if(seq_min == 0){
			if(decode_qitem_key(it->key(), NULL, &seq_min) == -1){
				// or just delete it?
				error = true;
				break;
			}
		}
		if(decode_qitem_key(it->key(), NULL, &seq_max) == -1){
			error = true;
			break;
		}
		count ++;
	}
	delete it;
	if(error){
		return -1;
	}
	
	if(count == 0){
		this->writer->Delete(encode_qsize_key(name));
		qdel_one(this, name, QFRONT_SEQ);
		qdel_one(this, name, QBACK_SEQ);
	}else{
		this->writer->Put(encode_qsize_key(name), Bytes((char *)&count, sizeof(count)));
		qset_one(this, name, QFRONT_SEQ, Bytes(&seq_min, sizeof(seq_min)));
		qset_one(this, name, QBACK_SEQ, Bytes(&seq_max, sizeof(seq_max)));
	}
		
	leveldb::Status s = writer->commit();
	if(!s.ok()){
		log_error("Write error!");
		return -1;
	}
	return 0;
}

}; // end namespace ssdb