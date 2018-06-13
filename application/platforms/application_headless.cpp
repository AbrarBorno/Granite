/* Copyright (c) 2017-2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "application.hpp"
#include "application_events.hpp"
#include "vulkan.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>
#include "stb_image_write.h"
#include "cli_parser.hpp"
#include "os.hpp"
#include "rapidjson_wrapper.hpp"
#include <limits.h>
#include "dynamic_library.hpp"
#include "hw_counters/hw_counter_interface.h"
using namespace rapidjson;

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

using namespace std;
using namespace Vulkan;
using namespace Util;

namespace Granite
{
class FrameWorker
{
public:
	FrameWorker();
	~FrameWorker();

	void wait();
	void set_work(function<void ()> work);

private:
	thread thr;
	condition_variable cond;
	mutex cond_lock;
	function<void ()> func;
	bool working = false;
	bool dead = false;

	void thread_loop();
};

FrameWorker::FrameWorker()
{
	thr = thread(&FrameWorker::thread_loop, this);
}

void FrameWorker::wait()
{
	unique_lock<mutex> u{cond_lock};
	cond.wait(u, [this]() -> bool {
		return !working;
	});
}

void FrameWorker::set_work(function<void()> work)
{
	wait();
	func = move(work);
	unique_lock<mutex> u{cond_lock};
	working = true;
	cond.notify_one();
}

void FrameWorker::thread_loop()
{
	for (;;)
	{
		{
			unique_lock<mutex> u{cond_lock};
			cond.wait(u, [this]() -> bool {
				return working || dead;
			});

			if (dead)
				return;
		}

		if (func)
			func();

		lock_guard<mutex> holder{cond_lock};
		working = false;
		cond.notify_one();
	}
}

FrameWorker::~FrameWorker()
{
	{
		lock_guard<mutex> holder{cond_lock};
		dead = true;
		cond.notify_one();
	}

	if (thr.joinable())
		thr.join();
}

struct WSIPlatformHeadless : Vulkan::WSIPlatform
{
public:
	WSIPlatformHeadless(unsigned width, unsigned height)
		: width(width), height(height)
	{
		if (!Context::init_loader(nullptr))
			throw runtime_error("Failed to initialize Vulkan loader.");

		EventManager::get_global().dequeue_all_latched(ApplicationLifecycleEvent::get_type_id());
		EventManager::get_global().enqueue_latched<ApplicationLifecycleEvent>(ApplicationLifecycle::Stopped);
		EventManager::get_global().dequeue_all_latched(ApplicationLifecycleEvent::get_type_id());
		EventManager::get_global().enqueue_latched<ApplicationLifecycleEvent>(ApplicationLifecycle::Paused);
		EventManager::get_global().dequeue_all_latched(ApplicationLifecycleEvent::get_type_id());
		EventManager::get_global().enqueue_latched<ApplicationLifecycleEvent>(ApplicationLifecycle::Running);
	}

	~WSIPlatformHeadless() override
	{
		release_resources();
		if (hw_counter_handle)
			hw_counter_iface.destroy(hw_counter_handle);
	}

	void release_resources() override
	{
		for (auto &thread : worker_threads)
			thread->wait();

		EventManager::get_global().dequeue_all_latched(ApplicationLifecycleEvent::get_type_id());
		EventManager::get_global().enqueue_latched<ApplicationLifecycleEvent>(ApplicationLifecycle::Paused);
		EventManager::get_global().dequeue_all_latched(ApplicationLifecycleEvent::get_type_id());
		EventManager::get_global().enqueue_latched<ApplicationLifecycleEvent>(ApplicationLifecycle::Stopped);

		swapchain_images.clear();
		readback_buffers.clear();
		acquire_semaphore.clear();
		readback_fence.clear();
	}

	bool alive(Vulkan::WSI &) override
	{
		return frames < max_frames;
	}

	void poll_input() override
	{
		get_input_tracker().dispatch_current_state(get_frame_timer().get_frame_time());
	}

	void enable_png_readback(string base_path)
	{
		png_readback = base_path;
	}

	vector<const char *> get_instance_extensions() override
	{
		return {};
	}

	VkSurfaceKHR create_surface(VkInstance, VkPhysicalDevice) override
	{
		return VK_NULL_HANDLE;
	}

	uint32_t get_surface_width() override
	{
		return width;
	}

	uint32_t get_surface_height() override
	{
		return height;
	}

	void notify_resize(unsigned width, unsigned height)
	{
		resize = true;
		this->width = width;
		this->height = height;
	}

	void set_max_frames(unsigned max_frames)
	{
		this->max_frames = max_frames;
	}

	bool has_external_swapchain() override
	{
		return true;
	}

	void init(Application *app)
	{
		this->app = app;

		auto &wsi = app->get_wsi();
		wsi.init_external_context(make_unique<Context>(nullptr, 0, nullptr, 0));

		auto &device = wsi.get_device();

		auto info = ImageCreateInfo::render_target(width, height, VK_FORMAT_R8G8B8A8_SRGB);
		info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		info.misc = IMAGE_MISC_CONCURRENT_QUEUE_GRAPHICS_BIT |
		            IMAGE_MISC_CONCURRENT_QUEUE_TRANSFER_BIT;

		BufferCreateInfo readback = {};
		readback.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		readback.domain = BufferDomain::CachedHost;
		readback.size = width * height * sizeof(uint32_t);

		for (unsigned i = 0; i < SwapchainImages; i++)
		{
			swapchain_images.push_back(device.create_image(info, nullptr));
			readback_buffers.push_back(device.create_buffer(readback, nullptr));
			acquire_semaphore.push_back(Semaphore(nullptr));
			worker_threads.push_back(make_unique<FrameWorker>());
			readback_fence.push_back({});
		}

		for (auto &swap : swapchain_images)
			swap->set_swapchain_layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

		wsi.init_external_swapchain(swapchain_images);
	}

	void set_time_step(double t)
	{
		time_step = t;
	}

	void begin_frame()
	{
		auto &wsi = app->get_wsi();
		wsi.set_external_frame(index, acquire_semaphore[index], time_step);
		acquire_semaphore[index].reset();
		worker_threads[index]->wait();
	}

	void wait_workers()
	{
		for (auto &worker : worker_threads)
			worker->wait();
	}

	void end_frame()
	{
		auto &wsi = app->get_wsi();
		auto &device = wsi.get_device();
		auto release_semaphore = wsi.get_external_release_semaphore();

		if (release_semaphore && release_semaphore->get_semaphore() != VK_NULL_HANDLE)
		{
			if (next_readback_cb)
			{
				device.add_wait_semaphore(CommandBuffer::Type::AsyncTransfer, release_semaphore,
				                          VK_PIPELINE_STAGE_TRANSFER_BIT, true);

				auto cmd = device.request_command_buffer(CommandBuffer::Type::AsyncTransfer);
				swapchain_images[index]->set_layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

				cmd->copy_image_to_buffer(*readback_buffers[index], *swapchain_images[index],
				                          0, {}, {width, height, 1},
				                          0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});

				cmd->barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);

				device.submit(cmd, &readback_fence[index], 1, &acquire_semaphore[index]);

				worker_threads[index]->set_work([cb = next_readback_cb, index = this->index]() {
					cb(index);
				});
				next_readback_cb = {};
			}
			else if (!png_readback.empty())
			{
				device.add_wait_semaphore(CommandBuffer::Type::AsyncTransfer, release_semaphore,
				                          VK_PIPELINE_STAGE_TRANSFER_BIT, true);

				auto cmd = device.request_command_buffer(CommandBuffer::Type::AsyncTransfer);
				swapchain_images[index]->set_layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

				cmd->copy_image_to_buffer(*readback_buffers[index], *swapchain_images[index],
				                          0, {}, {width, height, 1},
				                          0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});

				cmd->barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);

				device.submit(cmd, &readback_fence[index], 1, &acquire_semaphore[index]);

				worker_threads[index]->set_work([this, index = this->index, frame = this->frames]() {
					dump_frame(frame, index);
				});
			}
			else
			{
				acquire_semaphore[index] = release_semaphore;
			}
		}
		release_semaphore.reset();
		index = (index + 1) % SwapchainImages;
		frames++;
	}

	void set_next_readback(const std::string &path)
	{
		next_readback_cb = [this, path](unsigned index) {
			auto &wsi = app->get_wsi();
			auto &device = wsi.get_device();

			readback_fence[index]->wait();
			readback_fence[index].reset();

			auto *ptr = static_cast<uint32_t *>(device.map_host_buffer(*readback_buffers[index], MEMORY_ACCESS_READ_WRITE));
			for (unsigned i = 0; i < width * height; i++)
				ptr[i] |= 0xff000000u;

			if (!stbi_write_png(path.c_str(), width, height, 4, ptr, width * 4))
				LOGE("Failed to write PNG to disk.\n");
			device.unmap_host_buffer(*readback_buffers[index]);
		};
	}

	void wait_threads()
	{
		for (auto &thread : worker_threads)
			thread->wait();
	}

	void setup_hw_counter_lib(const char *path)
	{
		try
		{
			hw_counter_lib = DynamicLibrary(path);
			auto *get_iface = hw_counter_lib.get_symbol<get_hw_counter_interface_t>("get_hw_counter_interface");
			if (!get_iface)
			{
				LOGE("Count not find symbol for HW counter interface!\n");
				return;
			}

			if (!get_iface(&hw_counter_iface))
			{
				LOGE("Failed to get HW counter interface!\n");
				return;
			}

			hw_counter_handle = hw_counter_iface.create();
			if (!hw_counter_handle)
			{
				LOGE("Failed to create HW counter handle!\n");
				return;
			}
		}
		catch (const std::exception &e)
		{
			LOGE("Failed to load HW counter library: %s\n", e.what());
		}
	}

	bool get_counters(hw_counter &counter)
	{
		if (!hw_counter_handle)
			return false;
		return hw_counter_iface.wait_sample(hw_counter_handle, &counter);
	}

private:
	unsigned width = 0;
	unsigned height = 0;
	unsigned frames = 0;
	unsigned max_frames = UINT_MAX;
	unsigned index = 0;
	double time_step = 0.01;
	string png_readback;
	enum { SwapchainImages = 4 };

	vector<ImageHandle> swapchain_images;
	vector<BufferHandle> readback_buffers;
	vector<Semaphore> acquire_semaphore;
	vector<Fence> readback_fence;
	vector<unique_ptr<FrameWorker>> worker_threads;
	std::function<void (unsigned)> next_readback_cb;

	void dump_frame(unsigned frame, unsigned index)
	{
		auto &wsi = app->get_wsi();
		auto &device = wsi.get_device();

		readback_fence[index]->wait();
		readback_fence[index].reset();

		LOGI("Dumping frame: %u (index: %u)\n", frame, index);

		auto *ptr = static_cast<uint32_t *>(device.map_host_buffer(*readback_buffers[index], MEMORY_ACCESS_READ_WRITE));
		for (unsigned i = 0; i < width * height; i++)
			ptr[i] |= 0xff000000u;

		char buffer[64];
		sprintf(buffer, "_%05u.png", frame);
		auto path = png_readback + buffer;
		if (!stbi_write_png(path.c_str(), width, height, 4, ptr, width * 4))
			LOGE("Failed to write PNG to disk.\n");
		device.unmap_host_buffer(*readback_buffers[index]);
	}

	Application *app = nullptr;
	DynamicLibrary hw_counter_lib;
	hw_counter_interface hw_counter_iface;
	hw_counter_handle_t *hw_counter_handle = nullptr;
};

void application_dummy()
{
}
}

static void print_help()
{
	LOGI("[--png-path <path>] [--stat <output.json>]\n"
	     "[--fs-assets <path>] [--fs-cache <path>] [--fs-builtin <path>]\n"
	     "[--png-reference-path <path>] [--frames <frames>] [--width <width>] [--height <height>] [--time-step <step>] [--hw-counter-lib <lib>].\n");
}

#ifdef _WIN32
int CALLBACK WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
#else
int main(int argc, char *argv[])
#endif
{
#ifdef _WIN32
	int argc;
	wchar_t **wide_argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	vector<char *> argv_buffer(argc + 1);
	char **argv = nullptr;

	if (wide_argv)
	{
		argv = argv_buffer.data();
		for (int i = 0; i < argc; i++)
		{
			auto length = wcslen(wide_argv[i]);
			argv_buffer[i] = new char[length + 1];
			size_t num_converted;
			wcstombs_s(&num_converted, argv_buffer[i], length + 1, wide_argv[i], length + 1);
		}
	}
#endif

	using namespace Granite;
	if (argc < 1)
		return 1;

	struct Args
	{
		string png_path;
		string png_reference_path;
		string stat;
		string assets;
		string cache;
		string builtin;
		string hw_counter_lib;
		unsigned max_frames = UINT_MAX;
		unsigned width = 1280;
		unsigned height = 720;
		double time_step = 0.01;
	} args;

	std::vector<char *> filtered_argv;
	filtered_argv.push_back(argv[0]);

	CLICallbacks cbs;
	cbs.add("--frames", [&](CLIParser &parser) { args.max_frames = parser.next_uint(); });
	cbs.add("--width", [&](CLIParser &parser) { args.width = parser.next_uint(); });
	cbs.add("--height", [&](CLIParser &parser) { args.height = parser.next_uint(); });
	cbs.add("--time-step", [&](CLIParser &parser) { args.time_step = parser.next_double(); });
	cbs.add("--png-path", [&](CLIParser &parser) { args.png_path = parser.next_string(); });
	cbs.add("--png-reference-path", [&](CLIParser &parser) { args.png_reference_path = parser.next_string(); });
	cbs.add("--fs-assets", [&](CLIParser &parser) { args.assets = parser.next_string(); });
	cbs.add("--fs-builtin", [&](CLIParser &parser) { args.builtin = parser.next_string(); });
	cbs.add("--fs-cache", [&](CLIParser &parser) { args.cache = parser.next_string(); });
	cbs.add("--stat", [&](CLIParser &parser) { args.stat = parser.next_string(); });
	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--hw-counter-lib", [&](CLIParser &parser) { args.hw_counter_lib = parser.next_string(); });
	cbs.default_handler = [&](const char *arg) { filtered_argv.push_back(const_cast<char *>(arg)); };
	cbs.error_handler = [&]() { print_help(); };
	CLIParser parser(move(cbs), argc - 1, argv + 1);
	parser.ignore_unknown_arguments();
	if (!parser.parse())
		return 1;
	else if (parser.is_ended_state())
		return 0;

	filtered_argv.push_back(nullptr);

	if (!args.assets.empty())
		Filesystem::get().register_protocol("assets", make_unique<OSFilesystem>(args.assets));
	if (!args.builtin.empty())
		Filesystem::get().register_protocol("builtin", make_unique<OSFilesystem>(args.builtin));
	if (!args.cache.empty())
		Filesystem::get().register_protocol("cache", make_unique<OSFilesystem>(args.cache));

	auto app = unique_ptr<Application>(Granite::application_create(int(filtered_argv.size() - 1), filtered_argv.data()));

#ifdef _WIN32
	for (auto &arg : argv_buffer)
		delete[] arg;
#endif

	if (app)
	{
		auto platform = make_unique<WSIPlatformHeadless>(args.width, args.height);
		auto *p = platform.get();

		if (!args.hw_counter_lib.empty())
			p->setup_hw_counter_lib(args.hw_counter_lib.c_str());

		if (!app->init_wsi(move(platform)))
			return 1;

		if (!args.png_path.empty())
			p->enable_png_readback(args.png_path);
		p->set_max_frames(args.max_frames);
		p->set_time_step(args.time_step);
		p->init(app.get());

		// Run warm-up frame.
		if (app->poll())
		{
			p->begin_frame();
			app->run_frame();
			p->end_frame();
		}

		p->wait_threads();
		app->get_wsi().get_device().wait_idle();

		hw_counter start_counter, end_counter;
		bool has_start_counters = p->get_counters(start_counter);

		auto start_time = get_current_time_nsecs();
		unsigned rendered_frames = 0;
		while (app->poll())
		{
			p->begin_frame();
			app->run_frame();
			p->end_frame();
			rendered_frames++;
		}

		p->wait_threads();
		app->get_wsi().get_device().wait_idle();

		bool has_end_counters = p->get_counters(end_counter);

		auto end_time = get_current_time_nsecs();

		if (rendered_frames)
		{
			double usec = 1e-3 * double(end_time - start_time) / rendered_frames;
			LOGI("Average frame time: %.3f usec\n", usec);

			if (!args.stat.empty())
			{
				Document doc;
				doc.SetObject();
				auto &allocator = doc.GetAllocator();

				doc.AddMember("averageFrameTimeUs", usec, allocator);
				doc.AddMember("gpu", StringRef(app->get_wsi().get_context().get_gpu_props().deviceName), allocator);
				doc.AddMember("driverVersion", app->get_wsi().get_context().get_gpu_props().driverVersion, allocator);

				if (has_start_counters && has_end_counters)
				{
					doc.AddMember("gpuCycles", (end_counter.gpu_cycles - start_counter.gpu_cycles) / rendered_frames, allocator);
					doc.AddMember("bandwidthRead", (end_counter.bandwidth_read - start_counter.bandwidth_read) / rendered_frames, allocator);
					doc.AddMember("bandwidthWrite", (end_counter.bandwidth_write - start_counter.bandwidth_write) / rendered_frames, allocator);
				}

				StringBuffer buffer;
				PrettyWriter<StringBuffer> writer(buffer);
				//Writer<StringBuffer> writer(buffer);
				doc.Accept(writer);

				if (!Filesystem::get().write_string_to_file(args.stat, buffer.GetString()))
					LOGE("Failed to write stat file to disk.\n");
			}
		}

		if (!args.png_reference_path.empty())
		{
			p->set_next_readback(args.png_reference_path);
			p->begin_frame();
			app->run_frame();
			p->end_frame();
		}

		p->wait_threads();
		return 0;
	}
	else
		return 1;
}
