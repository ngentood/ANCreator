/**
 * @file ANFuby.hpp
 * @brief Modern C++23 Native Window & Overlay Management for Android.
 * 
 * @section License
 * MIT License (c)
 */

#pragma once

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_activity.h>
#include <android/native_window_jni.h>

#include <expected>
#include <string_view>
#include <memory>
#include <optional>
#include <format>
#include <source_location>

namespace fuby {

    // --- Types & Constants ---

    enum class log_level { debug = 3, info = 4, warn = 5, error = 6 };

    enum class error_code {
        jni_env_invalid,
        jni_exception,
        class_not_found,
        method_not_found,
        surface_control_failed,
        native_window_failed,
        display_query_failed
    };

    /** @brief Flags for hardware-level surface control. */
    enum class surface_flags : uint32_t {
        none            = 0,
        hidden          = 0x00000004,
        skip_screenshot = 0x00000040,
        secure          = 0x00000080, ///< Anti-recording / Anti-screenshot
        opaque          = 0x00000400
    };

    struct error {
        error_code code;
        std::string message;
        std::source_location location = std::source_location::current();
        std::string to_string() const {
            return std::format("{} ({}:{}:{})", message, location.file_name(), location.line(), location.function_name());
        }
    };

    template <typename... Args>
    void log(log_level level, std::string_view fmt, Args&&... args) {
        auto msg = std::vformat(fmt, std::make_format_args(args...));
        __android_log_print(static_cast<int>(level), "fuby", "%s", msg.c_str());
    }

    // --- JNI RAII ---

    struct jni_env {
        JNIEnv* env;
        JavaVM* vm;
        explicit jni_env(JavaVM* java_vm) : env(nullptr), vm(java_vm) {
            if (vm) vm->AttachCurrentThread(&env, nullptr);
        }
        [[nodiscard]] bool is_valid() const noexcept { return env != nullptr; }
        bool check_exception() const noexcept {
            if (env && env->ExceptionCheck()) {
                env->ExceptionDescribe(); env->ExceptionClear();
                return true;
            }
            return false;
        }
        auto operator->() const noexcept { return env; }
    };

    struct global_ref_deleter {
        JNIEnv* env;
        void operator()(jobject obj) const { if (env && obj) env->DeleteGlobalRef(obj); }
    };
    using unique_global_ref = std::unique_ptr<_jobject, global_ref_deleter>;

    inline unique_global_ref make_global_ref(jni_env& jni, jobject obj) {
        if (!obj) return {nullptr, {nullptr}};
        return unique_global_ref(jni->NewGlobalRef(obj), global_ref_deleter{jni.env});
    }

    // --- Internal Helpers ---

    namespace detail {
        struct jni_cache {
            jclass builder_cls;
            jmethodID builder_ctor, builder_set_name, builder_set_size, builder_set_parent, builder_set_flags, builder_build;
            jclass trans_cls;
            jmethodID trans_ctor, trans_set_alpha, trans_set_layer, trans_set_parent, trans_show, trans_hide, trans_remove, trans_apply;

            static jni_cache& get(jni_env& jni) {
                static jni_cache cache = [&jni]() {
                    jni_cache c{};
                    auto find_cls = [&](const char* name) { 
                        jclass cls = jni->FindClass(name);
                        if (jni.check_exception()) return (jclass)nullptr;
                        return (jclass)jni->NewGlobalRef(cls);
                    };
                    
                    auto get_mid = [&](jclass cls, const char* name, const char* sig) {
                        if (!cls) return (jmethodID)nullptr;
                        jmethodID mid = jni->GetMethodID(cls, name, sig);
                        if (jni->ExceptionCheck()) jni->ExceptionClear();
                        return mid;
                    };

                    c.builder_cls = find_cls("android/view/SurfaceControl$Builder");
                    c.builder_ctor = get_mid(c.builder_cls, "<init>", "()V");
                    c.builder_set_name = get_mid(c.builder_cls, "setName", "(Ljava/lang/String;)Landroid/view/SurfaceControl$Builder;");
                    c.builder_set_size = get_mid(c.builder_cls, "setBufferSize", "(II)Landroid/view/SurfaceControl$Builder;");
                    if (!c.builder_set_size) c.builder_set_size = get_mid(c.builder_cls, "setSize", "(II)Landroid/view/SurfaceControl$Builder;");
                    
                    c.builder_set_parent = get_mid(c.builder_cls, "setParent", "(Landroid/view/SurfaceControl;)Landroid/view/SurfaceControl$Builder;");
                    c.builder_set_flags = get_mid(c.builder_cls, "setFlags", "(II)Landroid/view/SurfaceControl$Builder;");
                    c.builder_build = get_mid(c.builder_cls, "build", "()Landroid/view/SurfaceControl;");
                    
                    c.trans_cls = find_cls("android/view/SurfaceControl$Transaction");
                    c.trans_ctor = get_mid(c.trans_cls, "<init>", "()V");
                    c.trans_set_alpha = get_mid(c.trans_cls, "setAlpha", "(Landroid/view/SurfaceControl;F)Landroid/view/SurfaceControl$Transaction;");
                    c.trans_set_layer = get_mid(c.trans_cls, "setLayer", "(Landroid/view/SurfaceControl;I)Landroid/view/SurfaceControl$Transaction;");
                    
                    c.trans_set_parent = get_mid(c.trans_cls, "setParent", "(Landroid/view/SurfaceControl;Landroid/view/SurfaceControl;)Landroid/view/SurfaceControl$Transaction;");
                    if (!c.trans_set_parent) {
                        c.trans_set_parent = get_mid(c.trans_cls, "reparent", "(Landroid/view/SurfaceControl;Landroid/view/SurfaceControl;)Landroid/view/SurfaceControl$Transaction;");
                    }

                    c.trans_show = get_mid(c.trans_cls, "show", "(Landroid/view/SurfaceControl;)Landroid/view/SurfaceControl$Transaction;");
                    c.trans_hide = get_mid(c.trans_cls, "hide", "(Landroid/view/SurfaceControl;)Landroid/view/SurfaceControl$Transaction;");
                    c.trans_remove = get_mid(c.trans_cls, "remove", "(Landroid/view/SurfaceControl;)Landroid/view/SurfaceControl$Transaction;");
                    c.trans_apply = get_mid(c.trans_cls, "apply", "()V");
                    return c;
                }();
                return cache;
            }
        };

        struct display_helper {
            struct info { 
                int32_t width, height, rotation; 
                float refresh_rate;
            };

            static std::expected<info, error> get_current(jni_env& jni, ANativeActivity* activity) {
                try {
                    auto act_cls = jni->GetObjectClass(activity->clazz);
                    auto get_wm = jni->GetMethodID(act_cls, "getWindowManager", "()Landroid/view/WindowManager;");
                    auto wm = jni->CallObjectMethod(activity->clazz, get_wm);
                    auto wm_cls = jni->GetObjectClass(wm);
                    auto get_display = jni->GetMethodID(wm_cls, "getDefaultDisplay", "()Landroid/view/Display;");
                    auto display = jni->CallObjectMethod(wm, get_display);
                    
                    auto display_cls = jni->GetObjectClass(display);
                    auto get_rot = jni->GetMethodID(display_cls, "getRotation", "()I");
                    auto get_fps = jni->GetMethodID(display_cls, "getRefreshRate", "()F");
                    
                    auto metrics_cls = jni->FindClass("android/util/DisplayMetrics");
                    auto metrics_ctor = jni->GetMethodID(metrics_cls, "<init>", "()V");
                    auto metrics = jni->NewObject(metrics_cls, metrics_ctor);
                    auto get_real = jni->GetMethodID(display_cls, "getRealMetrics", "(Landroid/util/DisplayMetrics;)V");
                    jni->CallVoidMethod(display, get_real, metrics);
                    
                    return info {
                        jni->GetIntField(metrics, jni->GetFieldID(metrics_cls, "widthPixels", "I")),
                        jni->GetIntField(metrics, jni->GetFieldID(metrics_cls, "heightPixels", "I")),
                        90 * jni->CallIntMethod(display, get_rot),
                        jni->CallFloatMethod(display, get_fps)
                    };
                } catch (...) { return std::unexpected(error{error_code::display_query_failed, "Failed display query"}); }
            }
        };
    }

    /**
     * @class native_overlay
     * @brief Ultimate RAII Native Window manager with Re-parenting support.
     */
    class native_overlay {
    public:
        struct config {
            std::string_view name = "fuby_overlay";
            int32_t width = -1, height = -1, layer = 0x7FFFFFFE;
            float alpha = 1.0f;
            surface_flags flags = surface_flags::none;
            bool visible = true;
        };

        static std::expected<std::unique_ptr<native_overlay>, error> create(ANativeActivity* activity, const config& cfg) {
            jni_env jni(activity->vm);
            if (!jni.is_valid()) return std::unexpected(error{error_code::jni_env_invalid, "JNI Failure"});

            auto& cache = detail::jni_cache::get(jni);
            if (!cache.builder_cls || !cache.builder_ctor || !cache.builder_build) 
                return std::unexpected(error{error_code::class_not_found, "SurfaceControl classes/methods not found"});

            auto disp = detail::display_helper::get_current(jni, activity);
            if (!disp) return std::unexpected(disp.error());

            int32_t w = (cfg.width <= 0) ? disp->width : cfg.width;
            int32_t h = (cfg.height <= 0) ? disp->height : cfg.height;

            auto parent = get_root_sc(jni, activity);
            
            auto builder = jni->NewObject(cache.builder_cls, cache.builder_ctor);
            if (!builder) return std::unexpected(error{error_code::jni_exception, "Failed to create SC Builder"});

            if (cache.builder_set_name) jni->CallObjectMethod(builder, cache.builder_set_name, jni->NewStringUTF(cfg.name.data()));
            if (cache.builder_set_size) jni->CallObjectMethod(builder, cache.builder_set_size, w, h);
            if (parent && cache.builder_set_parent) jni->CallObjectMethod(builder, cache.builder_set_parent, parent.get());
            
            if (cfg.flags != surface_flags::none && cache.builder_set_flags) {
                jni->CallObjectMethod(builder, cache.builder_set_flags, static_cast<jint>(cfg.flags), static_cast<jint>(cfg.flags));
            }

            auto sc_obj = jni->CallObjectMethod(builder, cache.builder_build);
            if (!sc_obj) return std::unexpected(error{error_code::surface_control_failed, "SC Build failed"});

            auto sc = make_global_ref(jni, sc_obj);
            apply_tx(jni, sc.get(), cfg.alpha, cfg.layer, cfg.visible);

            auto surface_cls = jni->FindClass("android/view/Surface");
            if (!surface_cls) return std::unexpected(error{error_code::class_not_found, "Surface class not found"});

            auto surface_ctor = jni->GetMethodID(surface_cls, "<init>", "(Landroid/view/SurfaceControl;)V");
            if (!surface_ctor) return std::unexpected(error{error_code::method_not_found, "Surface constructor not found"});

            auto surface_obj = jni->NewObject(surface_cls, surface_ctor, sc.get());
            auto surface = make_global_ref(jni, surface_obj);

            auto window = ANativeWindow_fromSurface(jni.env, surface_obj);
            if (!window) return std::unexpected(error{error_code::native_window_failed, "NDK Window failure"});

            return std::unique_ptr<native_overlay>(new native_overlay(activity, std::move(sc), std::move(surface), std::move(parent), window, w, h, *disp));
        }

        /** @brief Destructor ensures surface is removed from the composition. */
        ~native_overlay() {
            if (sc_) {
                jni_env jni(activity_->vm);
                if (jni.is_valid()) {
                    auto& cache = detail::jni_cache::get(jni);
                    if (cache.trans_cls && cache.trans_ctor && cache.trans_remove && cache.trans_apply) {
                        auto tx = jni->NewObject(cache.trans_cls, cache.trans_ctor);
                        if (tx) {
                            jni->CallObjectMethod(tx, cache.trans_remove, sc_.get());
                            jni->CallVoidMethod(tx, cache.trans_apply);
                        }
                    }
                }
            }
            if (window_) ANativeWindow_release(window_);
        }

        /** @brief Vital: Synchronizes overlay with the current Activity root. */
        void ensure_visible() {
            jni_env jni(activity_->vm);
            auto current_root = get_root_sc(jni, activity_);
            
            bool reparent = false;
            if (current_root && last_parent_) {
                if (!jni->IsSameObject(current_root.get(), last_parent_.get())) reparent = true;
            } else if (current_root && !last_parent_) reparent = true;

            if (reparent) {
                log(log_level::debug, "Fuby: Re-parenting overlay to new ViewRootImpl");
                auto& cache = detail::jni_cache::get(jni);
                if (cache.trans_cls && cache.trans_ctor && cache.trans_set_parent && cache.trans_apply) {
                    auto tx = jni->NewObject(cache.trans_cls, cache.trans_ctor);
                    if (tx) {
                        if (jni->GetMethodID(cache.trans_cls, "reparent", "(Landroid/view/SurfaceControl;Landroid/view/SurfaceControl;)Landroid/view/SurfaceControl$Transaction;") == cache.trans_set_parent) {
                            jni->CallObjectMethod(tx, cache.trans_set_parent, sc_.get(), current_root.get());
                        } else {
                            // setParent signature might vary, but cache should have resolved it correctly
                            jni->CallObjectMethod(tx, cache.trans_set_parent, sc_.get(), current_root.get());
                        }
                        jni->CallVoidMethod(tx, cache.trans_apply);
                    }
                }
                last_parent_ = std::move(current_root);
            }
            set_visibility(true);
        }

        void set_visibility(bool v) { jni_env j(activity_->vm); apply_tx(j, sc_.get(), -1.0f, -1, v, true); }
        void set_alpha(float a)     { jni_env j(activity_->vm); apply_tx(j, sc_.get(), a, -1, std::nullopt); }
        
        [[nodiscard]] ANativeWindow* get_window() const noexcept { return window_; }
        [[nodiscard]] int32_t get_width() const noexcept { return width_; }
        [[nodiscard]] int32_t get_height() const noexcept { return height_; }
        [[nodiscard]] detail::display_helper::info get_display_info() { jni_env j(activity_->vm); return detail::display_helper::get_current(j, activity_).value_or(last_display_); }

    private:
        native_overlay(ANativeActivity* act, unique_global_ref sc, unique_global_ref surf, unique_global_ref parent, ANativeWindow* win, int32_t w, int32_t h, detail::display_helper::info d)
            : activity_(act), sc_(std::move(sc)), surface_(std::move(surf)), last_parent_(std::move(parent)), window_(win), width_(w), height_(h), last_display_(d) {}

        static void apply_tx(jni_env& jni, jobject sc, float a, int32_t l, std::optional<bool> v, bool only_v = false) {
            auto& c = detail::jni_cache::get(jni);
            if (!c.trans_cls || !c.trans_ctor || !c.trans_apply) return;
            auto tx = jni->NewObject(c.trans_cls, c.trans_ctor);
            if (!tx) return;
            if (!only_v) {
                if (a >= 0.0f && c.trans_set_alpha) jni->CallObjectMethod(tx, c.trans_set_alpha, sc, a);
                if (l >= 0 && c.trans_set_layer) jni->CallObjectMethod(tx, c.trans_set_layer, sc, l);
            }
            if (v.has_value()) {
                jmethodID show_hide = *v ? c.trans_show : c.trans_hide;
                if (show_hide) jni->CallObjectMethod(tx, show_hide, sc);
            }
            jni->CallVoidMethod(tx, c.trans_apply);
        }

        static unique_global_ref get_root_sc(jni_env& jni, ANativeActivity* activity) {
            try {
                auto act_cls = jni->GetObjectClass(activity->clazz);
                auto get_win_mid = jni->GetMethodID(act_cls, "getWindow", "()Landroid/view/Window;");
                if (jni->ExceptionCheck()) { jni->ExceptionClear(); return {nullptr, {nullptr}}; }
                
                auto win = jni->CallObjectMethod(activity->clazz, get_win_mid);
                if (!win) return {nullptr, {nullptr}};
                
                auto win_cls = jni->GetObjectClass(win);
                auto get_decor_mid = jni->GetMethodID(win_cls, "getDecorView", "()Landroid/view/View;");
                if (jni->ExceptionCheck()) { jni->ExceptionClear(); return {nullptr, {nullptr}}; }
                
                auto decor = jni->CallObjectMethod(win, get_decor_mid);
                if (!decor) return {nullptr, {nullptr}};
                
                auto decor_cls = jni->GetObjectClass(decor);
                auto get_root_mid = jni->GetMethodID(decor_cls, "getViewRootImpl", "()Landroid/view/ViewRootImpl;");
                if (jni->ExceptionCheck()) { jni->ExceptionClear(); return {nullptr, {nullptr}}; }
                
                auto root = jni->CallObjectMethod(decor, get_root_mid);
                if (!root) return {nullptr, {nullptr}};
                
                auto root_cls = jni->GetObjectClass(root);
                auto get_sc_mid = jni->GetMethodID(root_cls, "getSurfaceControl", "()Landroid/view/SurfaceControl;");
                if (jni->ExceptionCheck()) { jni->ExceptionClear(); return {nullptr, {nullptr}}; }
                
                auto sc = jni->CallObjectMethod(root, get_sc_mid);
                return make_global_ref(jni, sc);
            } catch (...) { return {nullptr, {nullptr}}; }
        }

        ANativeActivity* activity_;
        unique_global_ref sc_, surface_, last_parent_;
        ANativeWindow* window_;
        int32_t width_, height_;
        detail::display_helper::info last_display_;
    };
} // namespace fuby
