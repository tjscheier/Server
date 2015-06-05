/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/

#include "../StdAfx.h"

#include "mixer.h"

#include "../frame/frame.h"

#include "audio/audio_mixer.h"
#include "image/image_mixer.h"

#include <common/env.h>
#include <common/executor.h>
#include <common/diagnostics/graph.h>
#include <common/except.h>
#include <common/future.h>
#include <common/timer.h>

#include <core/frame/draw_frame.h>
#include <core/frame/frame_factory.h>
#include <core/frame/frame_transform.h>
#include <core/frame/pixel_format.h>
#include <core/video_format.h>

#include <boost/property_tree/ptree.hpp>

#include <tbb/concurrent_queue.h>
#include <tbb/spin_mutex.h>

#include <unordered_map>
#include <vector>

namespace caspar { namespace core {

struct mixer::impl : boost::noncopyable
{				
	spl::shared_ptr<diagnostics::graph> graph_;
	audio_mixer							audio_mixer_;
	spl::shared_ptr<image_mixer>		image_mixer_;
			
	executor executor_									{ L"mixer" };

public:
	impl(spl::shared_ptr<diagnostics::graph> graph, spl::shared_ptr<image_mixer> image_mixer) 
		: graph_(std::move(graph))
		, image_mixer_(std::move(image_mixer))
	{			
		graph_->set_color("mix-time", diagnostics::color(1.0f, 0.0f, 0.9f, 0.8f));
	}
	
	const_frame operator()(std::map<int, draw_frame> frames, const video_format_desc& format_desc)
	{		
		caspar::timer frame_timer;

		auto frame = executor_.invoke([=]() mutable -> const_frame
		{		
			try
			{
				detail::set_current_aspect_ratio(
						static_cast<double>(format_desc.square_width)
						/ static_cast<double>(format_desc.square_height));

				for (auto& frame : frames)
				{
					frame.second.accept(audio_mixer_);
					frame.second.transform().image_transform.layer_depth = 1;
					frame.second.accept(*image_mixer_);
				}
				
				auto image = (*image_mixer_)(format_desc);
				auto audio = audio_mixer_(format_desc);

				auto desc = core::pixel_format_desc(core::pixel_format::bgra);
				desc.planes.push_back(core::pixel_format_desc::plane(format_desc.width, format_desc.height, 4));
				return const_frame(std::move(image), std::move(audio), this, desc);	
			}
			catch(...)
			{
				CASPAR_LOG_CURRENT_EXCEPTION();
				return const_frame::empty();
			}	
		});		
				
		graph_->set_value("mix-time", frame_timer.elapsed()*format_desc.fps*0.5);

		return frame;
	}

	void set_master_volume(float volume)
	{
		executor_.begin_invoke([=]
		{
			audio_mixer_.set_master_volume(volume);
		}, task_priority::high_priority);
	}

	float get_master_volume()
	{
		return executor_.invoke([=]
		{
			return audio_mixer_.get_master_volume();
		}, task_priority::high_priority);
	}

	std::future<boost::property_tree::wptree> info() const
	{
		return make_ready_future(boost::property_tree::wptree());
	}
};
	
mixer::mixer(spl::shared_ptr<diagnostics::graph> graph, spl::shared_ptr<image_mixer> image_mixer) 
	: impl_(new impl(std::move(graph), std::move(image_mixer))){}
void mixer::set_master_volume(float volume) { impl_->set_master_volume(volume); }
float mixer::get_master_volume() { return impl_->get_master_volume(); }
std::future<boost::property_tree::wptree> mixer::info() const{return impl_->info();}
const_frame mixer::operator()(std::map<int, draw_frame> frames, const struct video_format_desc& format_desc){return (*impl_)(std::move(frames), format_desc);}
mutable_frame mixer::create_frame(const void* tag, const core::pixel_format_desc& desc) {return impl_->image_mixer_->create_frame(tag, desc);}
}}
