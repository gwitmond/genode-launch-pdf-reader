// Launch PDF Reader

// Monitor a Report_ROM for new pdf files
// and start and instance of mupdf for each.

// Copyright 2019
// By: Guido Witmond
// License: AGPL V3


/* Genode includes */
#include <base/signal.h>
#include <base/log.h>
#include <base/attached_rom_dataspace.h>
#include <base/component.h>
#include <util/xml_node.h>
#include <base/output.h>
#include <base/heap.h>

// Child includes
#include <init/child_policy.h>
#include <base/sleep.h>
#include <base/child.h>
#include <os/child_policy_dynamic_rom.h>

namespace Launch_pdf_reader {
	using namespace Genode;
	struct Main;
	struct Child;
}

// struct Child copied/adapted from repos/os/src/app/sequence/main.cc
struct Launch_pdf_reader::Child : Genode::Child_policy
{
	Genode::Env &_env;

	Heap _services_heap { _env.pd(), _env.rm() };

	Xml_node const _start_node;

	Name const _name = _start_node.attribute_value("name", Name());

	bool const _have_config = _start_node.has_sub_node("config");

	Binary_name _start_binary()
	{
		Binary_name name;
		try {
			_start_node.sub_node("binary").attribute("name").value(name);
			return name != "" ? name : _name;
		}
		catch (...) { return _name; }
	}

	Binary_name const _binary_name = _start_binary();

	Child_policy_dynamic_rom_file _config_policy {
		_env.rm(), "config", _env.ep().rpc_ep(), &_env.pd() };

	class Parent_service : public Genode::Parent_service
	{
		private:

			Registry<Parent_service>::Element _reg_elem;

		public:

			Parent_service(Registry<Parent_service> &registry, Env &env,
				       Service::Name const &name)
			:
				Genode::Parent_service(env, name), _reg_elem(registry, *this)
			{ }
	};

	Registry<Parent_service> _parent_services { };

	// TODO: change this comment (is it still needed?)
	/* queue a child reload from the async Parent interface */
	Signal_transmitter _exit_transmitter;

	Genode::Child _child { _env.rm(), _env.ep().rpc_ep(), *this };

	Child(Genode::Env &env,
	      Xml_node const &start_node,
	      Signal_context_capability exit_handler)
	:
		_env(env),
		_start_node(start_node),
		_exit_transmitter(exit_handler)
	{
		// Genode::log("start_node: ", start_node);

		if (_have_config) {
			Xml_node config_node = start_node.sub_node("config");
			config_node.with_raw_node([&] (char const *start, size_t length) {
				_config_policy.load(start, length); });
		}
	}

	~Child()
	{
		_parent_services.for_each([&] (Parent_service &service) {
			destroy(_services_heap, &service); });
	}


	/****************************
	 ** Child_policy interface **
	 ****************************/

	Name name() const override { return _name; }

	Binary_name binary_name() const override { return _binary_name; }

	/**
	 * Provide a "config" ROM if configured to do so,
	 * otherwise forward directly to the parent.
	 */
	Route resolve_session_request(Service::Name const &name,
				      Session_label const &label) override
	{
		auto route = [&] (Service &service) {
			return Route { .service = service,
				       .label   = label,
				       .diag    = Session::Diag() }; };

		if (_have_config) {
			Service *s =
				_config_policy.resolve_session_request(name, label);
			if (s)
				return route(*s);
		}

		Service &service = *new (_services_heap)
			Parent_service(_parent_services, _env, name);

		return route(service);
	}

	/**
	 * Only a single child is managed at a time so
	 * no additional PD management is required.
	 * TODO: This is NOT true, there can be multiple files. Do something about it!
	 */
	Pd_session	   &ref_pd()	   override { return _env.pd(); }
	Pd_session_capability ref_pd_cap() const override { return _env.pd_session_cap(); }

	/**
	 * Reap the child, whether succesful or not
	 */
	void exit(int exit_value) override
	{
		Genode::log("child::exit: ", exit_value);
		_exit_transmitter.submit();
	}

	/**
	 * TODO: respond to yield_response by withdrawing
	 * child quota and informing our parent.
	 */

	/**
	 * Upgrade child quotas from our quotas,
	 * otherwise request more quota from our parent.
	 */
	void resource_request(Parent::Resource_args const &args) override
	{
		Ram_quota ram = ram_quota_from_args(args.string());
		Cap_quota caps = cap_quota_from_args(args.string());

		Pd_session_capability pd_cap = _child.pd_session_cap();

		/* XXX: pretty simplistic math here */

		if (ram.value) {
			Ram_quota avail = _env.pd().avail_ram();
			if (avail.value > ram.value) {
				ref_pd().transfer_quota(pd_cap, ram);
			} else {
				ref_pd().transfer_quota(pd_cap, Ram_quota{avail.value >> 1});
				_env.parent().resource_request(args);
			}
		}

		if (caps.value) {
			Cap_quota avail = _env.pd().avail_caps();
			if (avail.value > caps.value) {
				ref_pd().transfer_quota(pd_cap, caps);
			} else {
				ref_pd().transfer_quota(pd_cap, Cap_quota{avail.value >> 1});
				_env.parent().resource_request(args);
			}
		}

		_child.notify_resource_avail();
	}

	/**
	 * Initialize the child Protection Domain session with half of
	 * the initial quotas of this parent component.
	 */
	void init(Pd_session &pd, Pd_session_capability pd_cap) override
	{
		pd.ref_account(ref_pd_cap());
		ref_pd().transfer_quota(pd_cap, Cap_quota{_env.pd().avail_caps().value >> 1});
		ref_pd().transfer_quota(pd_cap, Ram_quota{_env.pd().avail_ram().value >> 1});
	}
};


struct Launch_pdf_reader::Main
{
  	Genode::Env &_env;
	Genode::Attached_rom_dataspace _report_rom { _env, "report_new_pdf" };
	Genode::Heap _heap { _env.ram(), _env.rm() };

	Constructible<Launch_pdf_reader::Child> child { };
	Attached_rom_dataspace config_rom { _env, "config" };
	Xml_node const config_xml = config_rom.xml();

	void exit_child();
	Signal_handler<Main> exit_handler {
	  //    _env.ep(), *this, &Main::start_next_child };
	  _env.ep(), *this, &Main::exit_child };

	typedef Genode::String<100> Name;
	typedef Genode::String<100> Path;

	/**
	 * Signal handler that is invoked when the report_pdf is updated.
	 */
	void _handle_update();
	Genode::Signal_handler<Main> _update_handler {
		_env.ep(), *this, &Main::_handle_update
	};

	// Show the pdf (asynchronously)
	void _show_pdf(Path dir_path, Name file_name);


	Main(Genode::Env &env) : _env(env)
	{
		Genode::log("started");
		//Genode::log("config_xml: ", config_xml);
		_report_rom.sigh(_update_handler);
	}
};


void Launch_pdf_reader::Main::_handle_update()
{
	Genode::log("handle_update");
	_report_rom.update();
	Genode::Xml_node report_node = _report_rom.xml();
	//Genode::log("Report XML: ", report_node);
	try {
	  // Loop over the directory elements
	  report_node.for_each_sub_node([&] (Xml_node const &dir_node) {

	      Path dir_path;
	      dir_node.attribute("path").value(&dir_path);
	      Genode::log("dir path: ", dir_path);

		// Loop over the file nodes, if there are any.
		dir_node.for_each_sub_node([&] (Xml_node const &file_node) {

		    Name file_name;
		    file_node.attribute("name").value(&file_name);
		    Genode::log("file name: ", file_name);

		    // We have a PDF, show it!
		    _show_pdf(dir_path, file_name);
		  });
	    });
	} catch (Xml_node::Nonexistent_sub_node) {
	  Genode::log("XML PARSE ERROR");
	}

	// End of this iteration. Let the main process wait for the next update.
}

void Launch_pdf_reader::Main::exit_child() {
	Genode::log("exit_child handler");
	child.destruct();
}


void Launch_pdf_reader::Main::_show_pdf(Path dir_path, Name file_name) {
  Genode::log("Show PDF at ", dir_path, file_name);

	int next_xml_index = 0;
	try { while (true) {
		Xml_node sub_node = config_xml.sub_node(next_xml_index++);
		//Genode::log("sub_node: ", sub_node);
		if (sub_node.type() != "start")
			continue;
		child.construct(_env, sub_node, exit_handler);
		break;
	} }
	catch (Xml_node::Nonexistent_sub_node) {
		Genode::error("No <start> node, nothing to start.");
		_env.parent().exit(0);
	}
}

void Component::construct(Genode::Env &env) { static Launch_pdf_reader::Main main(env); }
