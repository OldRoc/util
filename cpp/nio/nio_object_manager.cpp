//
// Created by hujianzhe on 16-8-17.
//

#include "nio_object.h"
#include "nio_object_manager.h"

namespace Util {
NioObjectManager::NioObjectManager(const NioObjectManager& o) {}
NioObjectManager& NioObjectManager::operator=(const NioObjectManager& o) { return *this; }
NioObjectManager::NioObjectManager(void) {
	assert_true(rwlock_Create(&m_lock));
}

NioObjectManager::~NioObjectManager(void) {
	rwlock_Close(&m_lock);
}

size_t NioObjectManager::count(void) {
	rwlock_LockRead(&m_lock);
	size_t count = m_objects.size();
	rwlock_Unlock(&m_lock);
	return count;
}

void NioObjectManager::get(std::vector<NioObject*>& v) {
	rwlock_LockRead(&m_lock);
	v.reserve(m_objects.size());
	for (object_iter it = m_objects.begin(); it != m_objects.end(); ++it) {
		v.push_back(it->second);
	}
	rwlock_Unlock(&m_lock);
}

NioObject* NioObjectManager::get(FD_t fd) {
	NioObject* object = NULL;
	rwlock_LockRead(&m_lock);
	object_iter it = m_objects.find(fd);
	if (it != m_objects.end()) {
		object = it->second;
	}
	rwlock_Unlock(&m_lock);
	return object;
}

void NioObjectManager::add(NioObject* object) {
	std::pair<FD_t, NioObject*> item(object->fd(), object);
	rwlock_LockWrite(&m_lock);
	m_objects.insert(item);
	rwlock_Unlock(&m_lock);
}

void NioObjectManager::del(FD_t fd) {
	rwlock_LockWrite(&m_lock);
	m_objects.erase(fd);
	rwlock_Unlock(&m_lock);
}

int NioObjectManager::expire(NioObject* buf[], int n) {
	int i = 0;
	rwlock_LockWrite(&m_lock);
	for (object_iter iter = m_objects.begin(); iter != m_objects.end(); ) {
		NioObject* object = iter->second;
		if (object->checkValid()) {
			++iter;
			continue;
		}
		object->invalid();
		if (i < n) {
			iter = m_objects.erase(iter);
			buf[i++] = object;
		}
		else {
			++iter;
		}
	}
	rwlock_Unlock(&m_lock);
	return i;
}
}
