<?xml version="1.0" encoding="UTF-8"?>
<xsl:transform xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
	xmlns:xs="http://www.w3.org/2001/XMLSchema" version="4.0">
	
	<xsl:template name="xsl:initial-template">
		<xsl:result-document method="csv" csv-quote-character="'" csv-separator=";">
			<xsl:array select="'a', 'b', 'c;c'"/>
			<xsl:array select="'d', 'e', 'f;f'"/>
		</xsl:result-document>
	</xsl:template>
</xsl:transform>