<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
   <!-- Called as xsl:initial-template: the result is the boolean true. -->
   <xsl:template name="xsl:initial-template" as="xs:boolean"
                 xmlns:xs="http://www.w3.org/2001/XMLSchema">
      <xsl:sequence select="true()"/>
   </xsl:template>
</xsl:stylesheet>
