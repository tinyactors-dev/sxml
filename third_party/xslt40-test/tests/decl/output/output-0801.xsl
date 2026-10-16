<?xml version="1.0" encoding="UTF-8"?>
<t:transform xmlns:t="http://www.w3.org/1999/XSL/Transform"
	xmlns:xs="http://www.w3.org/2001/XMLSchema" version="4.0">
	
	<t:output method="xhtml" html-version="5" include-content-type="false" />
	
	<!-- Purpose: Test that encoding declaration is not inserted when XSLT version is 4 and serialization is XHTML 5 and include-content-type is set to false -->
	
	<t:template name="t:initial-template">
		<html><head /></html>
	</t:template>
</t:transform>