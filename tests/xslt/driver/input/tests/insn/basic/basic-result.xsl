<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
   <!-- A principal result and a result document, both serialized. -->
   <xsl:template name="main">
      <out/>
      <xsl:result-document href="part.txt" method="text">
         <part>text</part>
      </xsl:result-document>
   </xsl:template>
</xsl:stylesheet>
