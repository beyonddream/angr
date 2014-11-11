import simuvex

######################################
# __isoc99_scanf
######################################

class __isoc99_scanf(simuvex.SimProcedure):
	#pylint:disable=arguments-differ

	def analyze(self, fmt_str): #pylint:disable=unused-argument
		# TODO: Access different registers on different archs
		# TODO: handle symbolic and static modes
		fd = 0 # always stdin

		# TODO: Now we assume it's always '%s'
		dst = self.arg(1)
		length = 17 # TODO: Symbolic length
		plugin = self.state['posix']

		data = plugin.read(fd, length)
		self.state.store_mem(dst, data)
		return dst
