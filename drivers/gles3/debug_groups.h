#pragma once

#define KHR_DEBUG_GROUPS 1

#if KHR_DEBUG_GROUPS

struct ScopeGroup {
	ScopeGroup(GLuint scope_id, ssize_t name_len, const char name_str[]) {
        glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, scope_id, name_len, name_str);
    }
    ~ScopeGroup() {
        glPopDebugGroup();
    }
};

#define SCOPE() ScopeGroup _khr_func_scope(__LINE__, sizeof(__FUNCTION__), __FUNCTION__);
#define RANGE(NAME) ScopeGroup _khr_##NAME(__LINE__, sizeof(#NAME), #NAME);

#else

#define SCOPE()
#define RANGE(NAME)

#endif
