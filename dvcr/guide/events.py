#    Copyright (C) 2000-2001 Mike Bernson <mike@mlb.org>
#
#    This program is free software; you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation; either version 2 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with this program; if not, write to the Free Software
#    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#
from Tkinter import *
from string import *
import time
import marshal 
import Pmw
import ConfigParser

class Event:
	def __init__(self):
		self.channel = StringVar()
		self.title = StringVar()
		self.start_hour = IntVar()
		self.start_min = IntVar()
		self.start_ampm = StringVar()
		self.stop_hour = IntVar()
		self.stop_min = IntVar()
		self.stop_ampm = StringVar()
		self.day= IntVar()
		self.month = IntVar()
		self.year = IntVar()
		self.repeat = StringVar()
		self.group_data = None
		self.group_frame = None

	def __del__(self):
		self.group_data.destroy()

	def show(self):
		self.group_data.focus_force()

	def get_start(self):
		hour = self.start_hour.get()
		if hour == 12:
			hour = 0
		if lower(self.start_ampm.get()) == 'pm':
			hour = hour + 12

		current = (self.year.get(), 
			self.month.get(), self.day.get(),
			hour, self.start_min.get(), 0, 0, 0, -1)
		return time.mktime(current) - time.timezone

	def set_start(self, time_value):
		if self.get_start() == time_value:
			return

		if time_value == None:
			time_value = time() - time.timezone
			time_value = time_value - (time_value % (30 * 60))
			time_value = time_value + 30 * 60

		self.start_hour.set(time.strftime("%l",time.gmtime(time_value)))
		self.start_min.set(time.strftime("%M",time.gmtime(time_value)))
		self.start_ampm.set(time.strftime("%p",time.gmtime(time_value)))

		self.day.set(time.strftime("%d",time.gmtime(time_value)))
		self.month.set(time.strftime("%m",time.gmtime(time_value)))
		self.year.set(time.strftime("%g",time.gmtime(time_value)))

	def get_stop(self):
		hour = self.stop_hour.get()
		if hour == 12:
			hour = 0
		if lower(self.stop_ampm.get()) == 'pm':
			hour = hour + 12
		current = (self.year.get(), 
			self.month.get(), self.day.get(),
			hour, self.start_min.get(), 0, 0, 0, -1)

		return time.mktime(current) - time.timezone

	def set_stop(self, time_value):
		if self.get_stop() == time_value:
			return

		if time_value == None:
			self.stop_hour.set("")
			self.stop_min.set("")
			self.stop_ampm.set("")
		else:
			self.stop_hour.set(time.strftime("%l",time.gmtime(time_value)))
			self.stop_min.set(time.strftime("%M",time.gmtime(time_value)))
			self.stop_ampm.set(time.strftime("%p",time.gmtime(time_value)))

	def get_title(self):
		return self.title.get()

	def set_title(self, title):
		if self.title.get() == title:
			return
		if title == None:
			self.title.set("")
		else:
			self.title.set(title)


	def get_channel(self):
		return self.channel.get()
	
	def set_channel(self, channel):
		if self.channel.get() == channel:
			return

		if channel == None:
			self.channel.set("")
		else:
			self.channel.set(channel)

	def get_repeat(self):
		return self.repeat.get()

	def set_repeat(self, repeat):
		if self.repeat.get() == repeat:
			return

		if repeat == "":
			repeat = "Once"
		if repeat == None:
			self.repeat.set("Once")
		else:
			self.repeat.set(repeat)

	def withdraw(self):
		self.group_data.grid_forget()

	def display(self, row):
		self.group_data.grid(row=row, column=0, sticky='NSEW', 
			ipadx=4, ipady=4)

	def once(self):
		self.set_repeat("Once")

	def week(self):
		self.set_repeat("Monday-Friday")

	def weekly(self):
		self.set_repeat("Weekly")

	def daily(self):
		self.set_repeat("Daily")

	def delete(self):
		self.set_repeat("Delete")

	def group(self, root, row):
		self.group_data = Pmw.Group(root, 
			tag_textvariable=self.repeat,
			tag_pyclass=Menubutton)
		self.group_frame = self.group_data.interior()

		command_button = self.group_data.component('tag')
		command_button.menu = Menu(command_button)
		command_button.menu.add_command(label='Once',
			underline=0, command=self.once)
		command_button.menu.add_command(label='Monday-Friday', 
			underline=0, command=self.week)
		command_button.menu.add_command(label='Weekly', 
			underline=0, command=self.weekly)
		command_button.menu.add_command(label='Daily', 
			underline=0, command=self.daily)
		command_button.menu.add_command(label='Delete', 
			underline=0, command=self.delete)
		command_button['menu'] = command_button.menu

		date_frame = Frame(self.group_frame)

		month = Entry(date_frame,
			textvariable = self.month,
			width = 2)
		
		day = Entry(date_frame, 
			textvariable = self.day,
			width = 2)

		year = Entry(date_frame, 
			textvariable = self.year,
			width = 2)

		label1 = Label(date_frame, text="/", width=1)
		label2 = Label(date_frame, text="/", width=1)

		month.grid(row=0, column=0)
		label1.grid(row=0, column=1)
		day.grid(row=0, column=2)
		label2.grid(row=0, column=3)
		year.grid(row=0, column=4)

		start_time_frame = Frame(self.group_frame)

		hour = Entry(start_time_frame,
			textvariable = self.start_hour,
			width = 2)
		
		min = Entry(start_time_frame, 
			textvariable = self.start_min,
			width = 2)

		ampm = Entry(start_time_frame,
			textvariable = self.start_ampm,
			width = 2)

		hour.grid(row=0, column=0, padx=3)
		min.grid(row=0, column=2, padx=3)
		ampm.grid(row=0, column=3, padx=3, ipadx=3)

		stop_time_frame = Frame(self.group_frame)

		hour = Entry(stop_time_frame,
			textvariable = self.stop_hour,
			width = 2)
		
		min = Entry(stop_time_frame, 
			textvariable = self.stop_min,
			width = 2)

		ampm = Entry(stop_time_frame,
			textvariable = self.stop_ampm,
			width = 2)

		hour.grid(row=0, column=0, padx=3)
		min.grid(row=0, column=2, padx=3)
		ampm.grid(row=0, column=3, padx=3, ipadx=3)

		dash = Label(self.group_frame,
			text = " - ",
			width = 3)
		date_frame.grid(row=0, column=0)
		start_time_frame.grid(row = 0, column=1)
		dash.grid(row=0, column = 2, sticky="NSEW")
		stop_time_frame.grid(row=0, column=3)

		channel = Entry(self.group_frame,
			textvariable=self.channel,
			width = 12)

		title = Entry(self.group_frame,
			textvariable=self.title,
			width=30)

		channel.grid(row=1, column=0, sticky="NSW")
		title.grid(row=1, column = 1, columnspan=3, sticky="NSW")

		self.group_data.grid(row=row, column=0, sticky='NSEW', 
			ipadx=4, ipady=4)

class EventList:
	def __init__(self, root):
		self.root = root
		self.config_data = ConfigParser.ConfigParser()
		self.config_data.read("/etc/dvcr")
		self.last_add = []

		self.record_event = {}
		self.event_groups = {}
		if root != None:
			self.dialog = Pmw.Dialog(root,
				title="Reording Events",
				buttons=("OK", "Add", "Cancel"),
				command=self.execute,
				defaultbutton='OK')
			self.dialog.withdraw()
	
			self.scroll_area = Pmw.ScrolledFrame(self.dialog.interior(),
				hscrollmode = 'dynamic')
			self.scroll_area.pack(fill=BOTH, expand=1, pady=4, padx=4)

		dir = self.config_string("global", "recorddir") + "/"
		self.file_name = dir + "directtv"

	def repeat(self, tag):
		changes = 0
		current_time = time.time() - time.timezone 

		start, end, channel, title, repeat = self.event(tag)

		if repeat == "Delete":
			self.delete(tag)
			return

		if end > current_time:
			return

		if repeat == 'Once':
			self.delete(tag)
			changes = changes + 1

		if repeat == "Daily":
			start = start + 24 * 60 * 60
			end = end + 24 * 60 * 60

		if repeat == "Weekly":
			start = start + 24 * 60 * 60 * 7
			end = end + 24 * 60 * 60 * 7

		if repeat == "Monday-Friday":
			start = start + 24 * 60 * 60
			end = end + 24 * 60 * 60

		dict = self.record_event[tag]
		dict['start'] = start
		dict['stop'] = end

		if self.root != None:
			event = self.event_groups[tag]
			event.set_start(start)
			event.set_stop(end)

	def load(self):
		try:
			file = open(self.file_name, "r")
		except:
			self.record_event = {}
			return

		self.record_event = marshal.load(file)
		row = 0
		for tag in self.tags():
			start, end, channel, title, repeat = \
				self.event(tag)
			if self.root != None:
				current = Event()
				self.event_groups[tag] = current
				current.set_start(start)
				current.set_stop(end)
				current.set_title(title)
				current.set_channel(channel)
				current.set_repeat(repeat)
				current.group(self.scroll_area.interior(), row)
			row = row + 1

#		self.repeat()

	def save(self):
		try:
			file = open(self.file_name, "w")
		except:
			print "could not create evnt file ", self.file_name
			return

		marshal.dump(self.record_event, file)

	def add(self, id, channel=None, start=None, stop=None, repeat=None, title=None):
		self.record_event[id] = { 
			"channel" : channel, 
			"start": start, 
			"stop" : stop,
			"repeat" : repeat,
			"title" : title
			}
		if self.root != None:
			current = Event()
			self.event_groups[id] = current
			current.set_start(start)
			current.set_stop(stop)
			current.set_title(title)
			current.set_channel(channel)
			current.set_repeat(repeat)
			current.group(self.scroll_area.interior(), 
				len(self.event_groups)-1)

		self.last_add.append(id)

	def last_delete(self):
		for tag in self.last_add:
			self.delete(tag)

	def delete(self, id):
		if self.record_event.has_key(id):
			del self.record_event[id]
		if self.event_groups.has_key(id):
			del self.event_groups[id]

	def event_cmp(self, a1, a2):
		diff = self.record_event[a1]['start'] - \
			self.record_event[a2]['start']
		if diff < 0:
			return -1
		if diff > 0:
			return 1
		return 0
		
	def tags(self):
		tags = self.record_event.keys()
		tags.sort(self.event_cmp)
		return tags

	def recorder(tag):
		if self.record_event.has_key(tag):
			dict = self.record_event[tag]
			if dict['repeat'] == 'Once':
				dict['repeat'] = "Deleted"
				return

			self.repeat(tag)

		

	def event(self, tag):
		if self.record_event.has_key(tag):
			dict = self.record_event[tag]
			return dict['start'], \
				dict['stop'], \
				dict['channel'], \
				dict['title'] , \
				dict['repeat']
		return None

	def list(self):
		for tag in self.tags():
			print self.event(tag)

	def config_string(self, section, name):
		try:
			return self.config_data.get(section, name)
		except:
			return None

	def execute(self, button):
		if button == 'OK':
			self.dialog.deactivate()
			self.last_add = []
			for tag in self.tags():
				dict = self.record_event[tag]
				event = self.event_groups[tag]

				if event.get_repeat() == "Delete":
					self.delete(tag)
				else:
					dict['channel'] = event.get_channel()
					dict['start'] = event.get_start()
					dict['stop'] = event.get_stop()
					dict['title'] = event.get_title()
					dict['repeat'] = event.get_repeat()
#			self.repeat()
			self.save()
			return

		if button == 'Cancel':
			self.dialog.deactivate()
			self.last_delete()
			self.last_add = []		
			for tag in self.tags():
				dict = self.record_event[tag]
				event = self.event_groups[tag]

				event.set_channel(dict['channel'])
				event.set_start(dict['start'])
				event.set_stop(dict['stop'])
				event.set_title(dict['title'])
				event.set_repeat(dict['repeat'])


		if button == "Add":
			self.add("x1")
			return


	def display_events(self, group_tag):

		if group_tag != None:
			if self.event_groups.has_key(group_tag):
				self.event_groups[group_tag].show()
		else:
			self.last_add = []

		for tag in self.tags():
			self.event_groups[tag].withdraw()
			self.repeat(tag)

		row = 0
		for tag in self.tags():
			self.event_groups[tag].display(row)
			row = row + 1

		result = self.dialog.activate()
