<?xml version="1.0" encoding="UTF-8"?>
<t:transform xmlns:t="http://www.w3.org/1999/XSL/Transform"
	xmlns:xs="http://www.w3.org/2001/XMLSchema" version="4.0">
	
	<t:output method="html" />
	
	<!-- Purpose: Test that encoding declaration is inserted using meta charset when XSLT version is 4 and serialization is HTML 5.0 (default when output/@html-version and output/@version are unset) and include-content-type is unset (equivalent to true) -->
	
	<t:template name="t:initial-template">
		<html><head /></html>
	</t:template>
</t:transform>